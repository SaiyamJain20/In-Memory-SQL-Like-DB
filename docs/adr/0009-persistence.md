# ADR 0009 — Persistence: checkpoint files, a write-ahead log, and a crash-testable file system

- **Status:** accepted
- **Date:** 2026-10-06

## Context
Until now a `Database` lived in memory. Phase 7 makes it durable: after `COMMIT`-equivalent success (every
statement is its own transaction) a power cut must not lose it, and a statement that was cut in half must not be
visible. The engine is *in-memory first*: tables are columnar row groups held in RAM and scanned zero-copy, so
persistence is "load at open, log while running, snapshot now and then" - not a buffer pool.

## Decision

**Files.** A database is a directory:

| File | Content |
|---|---|
| `LOCK` | advisory lock (one process per directory) |
| `checkpoint-<epoch>.cdb` | a complete snapshot of the catalog and every table at one instant |
| `wal-<epoch>.log` | the statements committed *after* checkpoint `<epoch>` was taken, one frame sequence |
| `*.tmp` | an unfinished checkpoint; deleted on open |

`epoch` is a 64-bit counter written as 16 hex digits. Recovery state = (newest checkpoint) + (every WAL with epoch >=
that checkpoint's, in order). No other file decides what the database is: there is no manifest to keep consistent.

**Commit.** Every state-changing statement (CREATE, DROP, INSERT, COPY) is one transaction made of one or more WAL
*frames*; the last frame carries a commit flag. The order is *validate and stage -> log -> fsync -> apply*: INSERT ...
SELECT and COPY already build their rows in a private staging table (Phase 4), so every error that can happen has
happened before anything is written, and the in-memory apply (a `Table::Merge`) cannot fail. Statements are serialised
by one commit mutex while logging and applying, so the log order is the apply order. If the log write or the fsync
fails, nothing was applied, the statement fails, and the WAL is *poisoned*: every later write fails until the database
is reopened (after a failed fsync the page cache can no longer be trusted - "fsyncgate"). Reads keep working.
`SyncMode::Full` (default) fsyncs every commit; `SyncMode::Off` leaves flushing to the OS (faster, a crash may lose the
last statements but never reorders or corrupts: recovery still yields a committed prefix).

**WAL frame.** `[u32 payload_len][u32 crc32c][u64 sequence][u32 flags][payload]`. The checksum covers the length, the
sequence, the flags and the payload. A file starts with a 16-byte header (`CDBWAL01`, epoch). Payload = operations:
`CreateTable(name, schema, row_group_size)`, `DropTable(name)`, `Append(table, rows)`; rows are written as typed column
vectors (validity bits, fixed-width values, length-prefixed strings). Large appends are split over frames of at most
16 MiB. **Recovery stops at the first frame that does not verify** (short, bad checksum, wrong sequence number) and
discards everything after it, together with any transaction whose commit frame was not reached; the file is truncated
to the last committed boundary before new frames are appended. A bad frame in a WAL that is *not* the newest is
corruption, not a torn tail (the old WAL was fsynced before the new one started), and is an error.

**Checkpoint file.** One file, written once, never modified:

```
header   : magic "CDBCKPT1", format version, epoch, header crc
blocks   : [u8 kind][u32 length][payload][u32 crc32c(kind, length, payload)]   (one per column segment, catalog, ...)
footer   : table directory (name, schema, row_group_size, per row group: row count and the file offset of each
           column segment's block), footer crc
trailer  : [u64 footer_offset][magic "CDBEND01"][u32 trailer crc]               (fixed 20 bytes at the end)
```

A column segment is stored *as it is in memory*: its encoding (constant, run-length, bit-packed, scaled double,
dictionary) and payload, validity bits and zone-map statistics, so loading is a read plus validation, not a re-encode,
and the file is as small as the in-memory table. Raw (uncompressed) segments store values directly. Every block
carries its own CRC-32C (hardware `crc32` instruction with a table fallback, equal results tested), is length-checked
against the file size before it is trusted, and a decoder validates structural invariants (bit widths, offsets inside
the payload, run ends increasing, dictionary codes below the dictionary size) so even a *checksummed but hostile* file
cannot make a scan read out of bounds. Loading is parallel: one task per row group.

**Checkpointing.** Under the commit mutex: take a snapshot of every table (cheap: immutable row groups plus a frozen
copy of each open tail), `fsync` the current WAL, create and fsync `wal-<e+1>.log` and the directory, and make it the
log that commits append to. Then, *without* the mutex (other statements keep committing into the new WAL): write
`checkpoint-<e+1>.cdb.tmp`, `fsync`, rename to its final name, `fsync` the directory, and only then delete
`checkpoint-<e>.cdb` and `wal-<e>.log`. Triggered by `CHECKPOINT`, when the WAL exceeds a size threshold, and on
open when the replayed log is large. The last, short row group of each table is restored as the table's open tail so
repeated restarts do not accumulate tiny row groups.

**Recovery** (`Database::Open`): lock the directory; delete `*.tmp`; take the newest `checkpoint-*.cdb` (**if it does
not verify, that is an error** - it was renamed into place only after an fsync, so it cannot be a torn write, and
guessing an older state could silently lose data); load it; replay each WAL with epoch >= it in order; truncate a torn
tail; open the newest WAL for appending. Recovery never writes anything except that truncation, so a crash *during*
recovery is just another crash.

**The file system is an interface.** All I/O goes through `FileSystem` / `FileHandle` (open, positional read and write,
append, truncate, fsync, rename, remove, directory fsync, list, lock). `PosixFileSystem` implements it with system
calls. `MemoryFileSystem` implements it in RAM *with a crash model*: each file has a durable image and a list of
unsynced writes; directory operations (create, rename, remove) are durable only after a directory fsync; `Crash(policy)`
returns the file system as it would be after power loss under a policy - drop everything unsynced, keep everything, or
keep a random prefix of the unsynced operations with the last write torn at a random byte. A `FaultInjector` decorator
makes the Nth operation throw `SimulatedCrash` (every write, fsync, rename, remove, truncate and directory fsync is a
crash point) or fail with an I/O error.

**How it is tested.**
- *Crash campaign:* run a scripted workload (several tables and types, NULLs, long strings, multi-frame appends, DROP and
  re-CREATE, INSERT ... SELECT, COPY, CHECKPOINT, a tiny automatic-checkpoint threshold) while crashing at *every* I/O
  operation, under every crash policy and several seeds; reopen from the surviving state and compare with a reference run
  of the same statements on an in-memory database: the state must equal the state after the last statement that returned
  success, or after the one in flight (atomic: either all or nothing) - never anything else. Then crash again *during
  recovery*, at every operation, and recover again.
- *I/O errors* injected at every operation: the statement fails, nothing is half applied, the database refuses further
  writes, and reopening gives a committed prefix.
- *Corruption:* every single-byte flip and every truncation of a valid checkpoint is detected; every flip / truncation of
  a WAL yields a committed prefix and never garbage; a libFuzzer target reads arbitrary bytes as a checkpoint and as a
  WAL (no crash, no UB, only `Error`).
- Concurrency (several sessions committing at once, then reopen) under ThreadSanitizer; the whole suite also runs against a
  persistent database in a `-persistent` mode.

## Consequences
- Open time is proportional to the data (everything is loaded into memory; there is no lazy paging).
- A WAL record stores raw rows, so a bulk `COPY` is written twice (log, then checkpoint), the usual cost of a log
  without a bulk-load fast path.
- One writer at a time (the commit mutex); readers are never blocked (snapshots).
- No `UPDATE` / `DELETE` exist yet, so the log has only three operations; adding them needs row ids and a change to the
  checkpoint (tombstones), not to the protocol.
- Single-file databases, encryption, compression of the log, group commit and incremental checkpoints are not done.

## Alternatives considered
- **A manifest file naming the current checkpoint** (LevelDB `CURRENT`): one more file to keep atomic with the others;
  "newest valid checkpoint by epoch" needs none.
- **Overwriting a checkpoint in place or a double-buffered header** (SQLite, LMDB): needs page-level atomicity
  assumptions; write-once files plus an atomic rename need only the rename.
- **A page cache / buffer manager:** the right design for data larger than memory and a different project; here the
  whole table is in memory.
- **Physiological or page-level logging:** needs pages; logical statements-as-rows is enough for append-only tables.
- **Logging the statement SQL text:** re-execution would depend on the engine version and on non-determinism; rows are
  self-contained.
- **fsync-less "eventually durable" as the only mode:** offered (`SyncMode::Off`) but not the default; the default is the
  one the tests prove.
