# Architecture

> **Status: design document.** Sections are tagged **[implemented]** or **[planned: Phase N]**.
> A tag is flipped only when the code and its tests land. If this document and the code disagree,
> the code is right and this file has a bug.

## Overview

```
 SQL text
    │
    ▼
 ┌─────────┐   ┌────────┐   ┌─────────┐   ┌───────────┐   ┌───────────┐
 │ Lexer / │──▶│  AST   │──▶│ Binder  │──▶│ Logical   │──▶│ Optimizer │
 │ Parser  │   │        │   │ (types, │   │ plan      │   │ (stats,   │
 └─────────┘   └────────┘   │  names) │   └───────────┘   │ pushdown, │
  Phase 3                    └─────────┘     Phase 3       │ join ord.)│
                                                           └─────┬─────┘
                                                      Phase 4 (rules), 8 (stats)
                                                                 ▼
                                                        ┌─────────────────┐
                                                        │ Physical plan   │
                                                        │ → pipelines     │  Phase 4
                                                        └────────┬────────┘
                                                                 ▼
        ┌──────────────────────────────────────────────────────────────────┐
        │ Execution: push-based pipelines, morsel-driven threads (Phase 6) │
        │   Source ─▶ [Filter ▶ Project ▶ Probe …] ─▶ Sink                 │
        │   data flows as DataChunks of ≤ 2048 rows (vectors)              │
        └───────────────┬─────────────────────────────────┬────────────────┘
                        │ expression kernels              │ scans
                        ▼                                 ▼
              typed, null-aware, selection-       ┌──────────────────────┐
              vector-aware, SIMD (Phase 5)        │ Columnar storage     │
                                                  │ RowGroup → Segments  │
                                                  │ encodings, zone maps │
                                                  │ Catalog              │
                                                  └──────────┬───────────┘
                                                             │ Phase 7
                                                             ▼
                                                  FileSystem ▸ WAL ▸ checkpoint
```

## Data model — [implemented: Phase 1]

### Vectors
A **Vector** holds up to `STANDARD_VECTOR_SIZE` (2048) values of one logical type. 2048 values of
8 bytes is 16 KiB: small enough that a handful of vectors stay in L1/L2 while an operator
pipeline runs over them, large enough to amortise interpretation overhead (virtual calls, type
dispatch) to ~nothing per row.

Formats:

| Format | Layout | Why |
|---|---|---|
| `FLAT` | contiguous values + validity mask | the default; auto-vectorisable loops |
| `CONSTANT` | one value, logically repeated | literals; avoids materialising `col + 1` operand |
| `DICTIONARY` | child vector + selection vector | zero-copy filter/slice, dictionary-encoded columns |

Kernels never branch on format in the inner loop. They convert any vector to a **unified view**
`(data*, sel*, validity*)` once per call and index `data[sel[i]]`; flat vectors get an identity
selection and take a specialised fast path. *The unified view (`UnifiedFormat`) is
[implemented: Phase 1]; the expression kernels that consume it are [implemented: Phase 4].*

### Nulls
Validity is a bitmask (1 bit/value). A vector with no nulls carries *no* mask, so the common case
costs a pointer check, not a bit test per row.

### Strings
16-byte string view: `{uint32 length; 12 bytes inline | 4-byte prefix + 8-byte pointer}`.
Strings ≤ 12 bytes need no heap at all; the 4-byte prefix makes most inequality comparisons and
`LIKE 'abc%'` checks resolve without a pointer dereference. Long strings live in a reference-counted
heap owned by the vector (or shared with the storage segment they came from).

### Selection vectors
Filters do not copy data. They emit a `SelectionVector` (array of row indices); downstream
operators read through it. A dictionary vector is exactly "child + selection", so slicing a chunk
by a filter is O(selected) pointer work.

## Storage — [implemented: Phase 2, encodings: Phase 5]
Tables are lists of **row groups** (60 vectors = 122,880 rows). Each row group holds one
**column segment** per column.

**Segments are immutable [implemented]**. A *raw* segment is laid out exactly like a flat vector
(values + validity bitmask + sealed string heap, each padded to a whole number of vectors), so a scan does no
work per value: `ColumnSegment::Scan` points the output vector at precomputed read-only `Buffer::View`s
(`Vector::ReferenceFlat`), with no copy and no allocation. Vectors obtained this way are read-only
(asserted), and `Vector::Reset` detaches from them instead of reusing them for writing.

**Encodings [implemented: Phase 5]** ([ADR 0007](adr/0007-segment-encodings-and-simd-dispatch.md)). When a
row group seals, each column segment is encoded if that makes it at most 70% of its raw size, otherwise it
stays raw: constant, RLE, bit-packed integers (frame of reference, or delta per vector for non-decreasing
data), scaled doubles (`n / 10^e`, verified bit for bit so it is lossless, with a raw fallback per vector),
and dictionary strings. Decoding is per 2048-row vector; constant vectors decode to CONSTANT vectors and
dictionary segments expose a DICTIONARY-format vector over one shared dictionary (no string is copied, and
hashing hashes each distinct entry once). The open tail is never encoded. On TPC-H SF1 the stored data
shrinks from 1,408 MB to 612 MB (lineitem 2.9x); the cost is decode time on scan-bound queries
([BENCHMARKS](BENCHMARKS.md)). `SetCompressionEnabled(false)` / `CDB_NO_COMPRESSION` keeps everything raw.

**Zone maps [implemented]**: every segment carries exact min / max / null count (doubles use the
NaN-last total order; string bounds are kept up to 64 bytes). `ColumnStats::CanSkip(op, c)` is
*sound* — it only answers "skip" when no row can satisfy `column <op> c` — and exact for
`<`, `<=`, `>`, `>=`, `<>`. A scan given `TableFilter`s skips whole row groups whose zone maps
rule them out; the Filter operator (Phase 4) still applies the real predicate to the rest.

**Writes and snapshots [implemented]**: a table is append-only. Appends fill a mutable
`RowGroupBuilder` (geometrically growing flat buffers, one `ColumnBuilder` per column); a full
builder is sealed into an immutable `RowGroup` without copying. `Table::Snapshot()` returns the
sealed groups plus a frozen copy of the open tail (cached until the next append; its long
strings are shared with the builder's append-only heap), so **readers get snapshot isolation
and never hold a lock while scanning**; this is also what lets Phase 6 hand row groups to
worker threads as morsels. A scan takes a projection (only requested columns are touched,
verified by per-segment scan counters in the tests; a zero-column projection still yields row
counts for `COUNT(*)`) and optional pruning filters.

**Catalog [implemented]**: case-insensitive, thread-safe name → table registry (`Catalog`,
owned by `Database`).

## SQL front end — [implemented: Phase 3]
`parser/` is a hand-written lexer and recursive-descent / precedence-climbing parser producing a
syntax-only AST. Every node records its source offset, and `ToString()` prints fully
parenthesised SQL, so *parse → print → parse is a fixpoint* (tested on a corpus, all 22 TPC-H
queries and fuzzed input). Hostile input cannot overflow the stack: nesting and left-deep chains
are depth-bounded and rejected with a positioned syntax error. `FormatErrorWithContext` renders
`LINE n:` plus a caret.

`planner/binder` turns the AST into a **logical plan** of relational operators over
`BoundExpr`s: name resolution with scopes (aliases, `USING`, derived tables), type inference with
explicit cast nodes, constant folding (including `DATE ± INTERVAL`), aggregate extraction with
SQL's GROUP BY / HAVING / ORDER BY rules, and lowering of `BETWEEN`/`IN`/`LIKE`/`CASE`. Columns
are referenced by ordinal into the operator's input. Semantics follow DuckDB; the deliberate
divergences are in [ADR 0003](adr/0003-semantics-and-divergences-from-duckdb.md).

`EvaluateScalar` is the single reference implementation of expression semantics
([ADR 0004](adr/0004-scalar-interpreter-as-reference-semantics.md)): it constant-folds during
binding and is the oracle the vectorized kernels are tested against. It is checked against DuckDB
on ~4,300 generated expressions (`tests/planner/golden_expression_test.cpp`).

`Connection::Query` parses, binds, optimizes, plans and executes: DDL, `INSERT … VALUES` and
`INSERT … SELECT` (atomic, via a staging table merged in one step), `COPY … FROM` (CSV, atomic the
same way), `EXPLAIN` (the optimized logical plan) and `SELECT` run. **12 of the 22 TPC-H queries
run** (and match DuckDB); the other 10 stop precisely at a subquery or `WITH` (Phase 8).

## Execution — [implemented: Phase 4; morsel-driven parallelism: Phase 6]
**Push-based pipelines** ([ADR 0005](adr/0005-push-pipelines-with-global-and-local-state.md)).
A query compiles to pipelines `source → streaming operators → sink`, run in dependency order.
Pipeline breakers (hash aggregate, join build, sort, top-N) are a sink in one pipeline and the
source of the next. Operators implement the full global/local state protocol - per-thread
accumulation in local state, merged in `Combine`, built in `Finalize` - and the tests drive it with
several local states; Phase 6 added the scheduler and morsel dispatch, not a rewrite. Streaming
operators can report `Finished` (a satisfied `LIMIT`), which stops the source being read.

**Operators.** Table scan (snapshot per query, zone-map pruning), `VALUES`, filter (selection
vectors; zero-copy dictionary output), projection, limit/offset, hash aggregate (also `DISTINCT`;
`COUNT/SUM/AVG/MIN/MAX` and their `DISTINCT` forms; integer `SUM` is exact in 128 bits and an error only if the total leaves `BIGINT`), `ORDER BY`
(stable) and top-N (prunes while consuming), hash join (inner / left / semi / anti, multi-key,
residual predicates, NULL keys never match; nested loop when there is no equality; output resumes
mid-chain so a probe row with many matches never overflows a chunk), result collector and INSERT.
`RIGHT` joins run as swapped `LEFT` joins; `FULL` is not supported yet; semi/anti exist in the
operator but nothing produces them until subquery unnesting (Phase 8).

**Data structures.** `ChunkStore` (append-only rows as flat chunks, gather by row id), `KeyIndex`
(open-addressing hash index assigning dense ids to distinct keys, NULLs equal), `GroupTable` (key
index + one struct-of-arrays state per aggregate, mergeable), and a chained join table (bucket
heads, `next` array, stored hashes, key store). Hashing follows the engine's equality (`0.0 = -0.0`,
all NaNs equal).

**Expressions.** `ExpressionExecutor` evaluates a bound expression a vector at a time with typed,
NULL-aware kernels that read through `UnifiedFormat`, so flat, constant and dictionary inputs all
work without flattening. `AND`/`OR`/`CASE`/`COALESCE` are lazy per row. Predicates take a separate
`Select` path that writes a selection vector directly, narrowing it between conjuncts. One
documented difference from the row-at-a-time interpreter: in selection position `a AND b` never
evaluates `b` for rows where `a` is NULL or FALSE (they cannot be TRUE), so a run-time error in `b`
on such a row is raised by the interpreter but not the executor; the differential test tolerates
exactly that case.

**SIMD kernels [implemented: Phase 5]** (`src/kernels/`, [ADR 0007](adr/0007-segment-encodings-and-simd-dispatch.md)).
AVX2 versions with scalar fallbacks, dispatched at run time (`__builtin_cpu_supports`, no `-march=native`):
compare-a-column-with-a-constant into a selection vector (used by the executor's `Select`), scaled-double
decode, ungrouped SUM/MIN/MAX, integer hashing. Each is tested against the scalar version and an
independent definition; results do not depend on the CPU (the one documented exception is the
re-association of ungrouped `SUM(DOUBLE)`). `CDB_NO_SIMD` forces the scalar path.

**Optimizer** ([ADR 0006](adr/0006-rule-based-optimizer.md)): filter pushdown (outer-join aware),
zone-map hints, greedy join ordering sized by distinct-value estimates from zone maps, OR
factoring, `LIMIT` below projections (top-N), column pruning. Statistics-based costing is Phase 8.

**Verification.** All 12 runnable TPC-H queries match DuckDB at SF0.01 / 0.1 / 1; ~240 queries in
`tests/sql/*.test` (expected results generated by DuckDB); random expressions executor vs
interpreter; random queries optimizer on vs off; every operator against a naive reference;
`tools/mutation_smoke.py`. Numbers vs DuckDB are in [BENCHMARKS](BENCHMARKS.md).

**Morsel-driven parallelism — [implemented: Phase 6]** ([ADR 0008](adr/0008-morsel-driven-parallelism.md)).
`Database` owns a fixed pool (`TaskScheduler`; one thread by default, `CDB_THREADS` / `SetThreads`, the
shell and the benchmark use every hardware thread). A job is `body(participant)` run by up to N threads;
the caller is participant 0, so nested and concurrent jobs cannot deadlock, and the first exception is
rethrown after all participants return. A pipeline runs on several threads only if its source, every
operator and its sink allow it (default: no; `LIMIT` says no). Each participant has its own local states
and claims *morsels* (8 vectors, never across a row group) from an atomic cursor, then hands its state
over in `Combine`:
- **aggregate:** per-thread `GroupTable`s merged by hash partition (from 32,768 groups; each partition
  merged by one task, no locks); integer `SUM` is exact in 128 bits so the result never depends on the
  thread count;
- **join:** per-thread row stores adopted whole, hashed in parallel and linked into bucket chains without
  atomics (radix scatter by bucket partition); probing was already per-thread;
- **sort:** parallel stable merge sort with co-rank slices, equal to the serial stable order; top-N
  prunes per thread first;
- **CSV:** mmap, record boundaries found in parallel (or by a serial quote scanner), whole row groups
  parsed, sealed and compressed per task, published in file order; same errors as the serial loader.

Chunks carry a *batch index* (morsel number), so `SELECT ... WHERE` over a scan and `INSERT ... SELECT`
keep table order on any thread count. Not defined with more than one thread: the order of groups,
of matches within one probe row, and of ties in a sort without a total key (none of which SQL promises).
Floating-point `SUM`/`AVG` re-associate. The whole test suite runs a second time in `-parallel` mode
(4 threads, one-vector morsels, every parallel threshold at 1) under debug, release, ASan and TSan.

## Error handling
See [ADR 0002](adr/0002-error-handling.md): exceptions for query-level errors at module
boundaries (parse/bind/runtime), no exceptions in inner loops; the public API converts them to a
result with an error state.

## Testing strategy
| Layer | Tool |
|---|---|
| Unit | GoogleTest, every format × type × null combination for data structures |
| SQL | `sqllogictest`-style files in `tests/sql/` (DuckDB-generated expected results) **[implemented]** |
| Differential | 12 TPC-H queries vs DuckDB (SF0.01 in the gate, SF0.1/SF1 by hand) **[implemented]**; random expressions vs the interpreter and random queries optimizer-on vs off **[implemented]**; random SQL fuzzing vs DuckDB [planned: Phase 8] |
| Fuzz | libFuzzer on parser, later on file-format readers |
| Concurrency | ThreadSanitizer in CI, the whole suite also in `-parallel` mode (4 threads, 1-vector morsels, all thresholds at 1) **[implemented]** |
| Crash safety | deterministic fault injection through the `FileSystem` interface (Phase 7) |
| Performance | Google Benchmark micro-benchmarks + TPC-H runner; results in `BENCHMARKS.md` |
