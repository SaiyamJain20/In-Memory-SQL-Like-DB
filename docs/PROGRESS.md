# Progress log

Append-only, newest entry last. One entry per milestone: what changed, what was run and the
result, numbers where relevant, and what is still broken or missing. Entries record what was
*observed*, not what was intended.

Reference machine for all numbers in this repo unless stated otherwise: AMD Ryzen 7 6800H
(8C/16T, AVX2, no AVX-512), 14 GB RAM, Ubuntu 24.04, GCC 13.3, Clang 18.1.

---

## 2026-10-06 — Phase 0: Foundation
**Branch:** `phase-0-foundation`

**Changed**
- Retired the legacy row-store prototype (history: `a254830`); rationale in ADR 0001.
- CMake build (C++20, `cdb` library + `cdb_tests`), presets `debug` / `release` / `asan` / `tsan`,
  `CDB_WERROR`, `CDB_SANITIZER`, runtime-assert switch (`CDB_ENABLE_ASSERTS` in Debug + sanitizers).
- GoogleTest 1.15.2 via FetchContent.
- `common/`: `Error` + `ErrorCode` (ADR 0002), `CDB_CHECK` / `CDB_ASSERT` / `CDB_UNREACHABLE`,
  version constant.
- Toolchain smoke tests (concepts, `std::span`, `std::format`, `<bit>`, `std::jthread`, atomic
  wait/notify) so GCC/Clang divergence surfaces as a test failure.
- `.clang-format`, `tools/check_format.sh` (pinned clang-format 23.1.2).
- GitHub Actions: format check; gcc-13 + clang-18 × debug + release; asan and tsan.
- Docs: ROADMAP, ARCHITECTURE, ADR 0001/0002, CLAUDE.md, README.

**Verified (local)**
| Check | Result |
|---|---|
| `debug` preset (gcc 13.3), 13 tests | 13/13 pass |
| `asan` preset (ASan+UBSan, gcc 13.3), 13 tests | 13/13 pass |
| `tsan` preset (ThreadSanitizer, gcc 13.3), 13 tests | 13/13 pass |
| `release` preset (gcc 13.3, `-O3`), 12 tests (assert death test compiled out) | 12/12 pass |
| clang 18.1 Debug, `-Werror`, 13 tests | 13/13 pass |
| `tools/check_format.sh` | OK (7 files) |

**Notes / known gaps**
- Clang on this machine needs `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13` (a GCC 14 dir
  without libstdc++ is picked otherwise). Documented in CLAUDE.md; applied in CI.
- TSan aborts on this kernel (high `vm.mmap_rnd_bits`) unless ASLR is disabled; the test binary
  is wrapped in `setarch -R` for TSan builds (see `tests/CMakeLists.txt`).
- No engine code yet; `release` + benchmarks are wired in Phase 1 when there is something to measure.

**CI (GitHub Actions, run 37373633236)** — all 7 jobs green: format, gcc-13 and clang-18 × debug
and release, asan (ASan+UBSan), tsan. The first attempt left six jobs unscheduled ("job was not
acquired by Runner of type hosted"): a GitHub runner-capacity failure, not a test failure; the
one job that did get a runner passed, and `gh run rerun --failed` then passed the other six.

**Phase 0 exit criteria met** → merged to `main`.

---

## 2026-10-06 — Phase 1: Core data model
**Branch:** `phase-1-core-data-model`

**Changed**
- `memory/`: `Buffer` (64-byte aligned, zeroed, 64 bytes of SIMD slack, ref-counted), `Arena`
  (bump allocator, stable pointers, dedicated blocks for large allocations, `Reset`).
- `types/`: `LogicalType`/`PhysicalType` (BOOLEAN, INTEGER, BIGINT, DOUBLE, DATE, VARCHAR; DATE
  shares INTEGER's physical type), 16-byte `string_t` (12-byte inline, 4-byte prefix, unsigned
  bytewise ordering), `date_t` + `Date` (proleptic Gregorian, years 1–9999, parse/format),
  `Value` (scalar with a documented total order: NaN last and equal to itself, -0.0 == +0.0).
- `vector/`: `ValidityMask` (bitmask, no storage when all valid), `SelectionVector`
  (+ shared read-only Identity/Zeros), `StringHeap`, `Vector` with FLAT / CONSTANT / DICTIONARY
  formats (`Slice`, `Flatten`, `Reference`, `Reset`, `SetConstant`, `ToUnified`, `Verify`),
  `VectorOps::Copy`, `DataChunk` (`Append`, `Slice`, `Flatten`, `Reset`).
- Tests infrastructure: shared random generators with adversarial strings/doubles; custom gtest
  `main` that makes the process non-dumpable (death tests went from ~24 s to ~1 s on this
  machine, where `core_pattern` pipes to apport and `RLIMIT_CORE=0` is ignored).
- `bench/cdb_bench` (Google Benchmark) and `tools/mutation_smoke.py`.

**Verified**
| Check | Result |
|---|---|
| `debug` (gcc 13.3, `-Werror`) | 151/151 |
| `asan` (ASan + UBSan, `halt_on_error`) | 151/151 |
| `tsan` | 151/151 |
| `release` (`-O3`; assert death tests compiled out) | 150/150 |
| clang 18.1 Debug, `-Werror` | 151/151 |
| `tools/check_format.sh` | OK (43 files) |
| `tools/mutation_smoke.py` | **15/15 mutations killed** (14 under `debug`, 1 under `asan`) |

Test design highlights: exhaustive `Date` round-trip for every day of years 1–9999 against
`std::chrono` (3.65 M days) and every `(start,count)` pair for `SetRangeValid`; `string_t`
comparison/equality against `std::string_view` over 160k random pairs engineered to collide on
prefixes and straddle the inline boundary (embedded NULs, bytes ≥ 0x80); a model-based
property test that applies 12 random structural operations (slice, flatten, reference, copy into
pre-populated destinations, reset-and-refill, set-constant) to 360 random vectors and compares
every row to a `std::vector<Value>` model after every step — including that vectors
`Reference()`d before a `Reset()` keep their old contents; death tests for every contract check.

**Found by the process (not by inspection)**
1. First benchmark showed `VectorOps::Copy` at 2.4 ns/row for plain INTEGER copies (~50× off
   `memcpy`); reworked into value-copy + validity-copy phases → 55× faster (numbers and caveats
   in `docs/BENCHMARKS.md`).
2. Mutation testing found a real test gap: nothing copied a NULL-free source over a destination
   that already contained NULLs. Added a targeted test and made the property test copy into
   pre-populated destinations at random offsets.

**Known gaps / deliberate limits**
- `Vector` capacity is capped at `kVectorSize` (2048); `Value` has no casts yet (Phase 3/4).
- No hashing of vectors yet (needed for aggregation/joins in Phase 4).
- `string_t::view()` on an inlined string points into the `string_t` object itself (documented in
  the header); Phase 4 kernels must take views from the stored element, not from a copy.

**Phase 1 exit criteria met** (tests for every format × type × null combination incl.
slice-of-slice; sanitizer clean; micro-benchmarks recorded).

**CI (GitHub Actions, run 37379450563)** — all 7 jobs green on the pushed branch: format,
gcc-13 and clang-18 × debug and release, asan (ASan+UBSan), tsan.

---

## 2026-10-06 — Phase 2: Columnar storage and catalog
**Branch:** `phase-2-columnar-storage`

**Changed**
- Vector-layer primitives for zero-copy scans: `Buffer::View` (read-only window that keeps its
  parent alive), `ValidityMask::FromBuffer`/`Resize`, `StringHeap::Seal`, `Vector::ReferenceFlat`,
  `VectorOps::CopyRows` (Copy generalised to raw destinations + a source offset). Mutating a
  mask or vector over read-only storage asserts; `Vector::Reset` detaches from read-only buffers.
- `storage/`: `ColumnStats` (exact min/max/null count, NaN-last total order, string bounds ≤ 64 B,
  sound `CanSkip` for = <> < <= > >=), `ColumnSegment` (immutable, zero-copy `Scan`),
  `ColumnBuilder`/`RowGroupBuilder` (geometric growth, seal without copying, tail snapshots),
  `RowGroup` (60 vectors), `Table` (append-only; `Snapshot()` = sealed groups + cached frozen tail),
  `TableScan` (projection incl. zero-column scans, zone-map group pruning).
- `catalog/Catalog` (case-insensitive, thread-safe) and `main/Database`.
- `bench/storage_bench.cpp`.

**Verified**
| Check | Result |
|---|---|
| `debug` (gcc 13.3, `-Werror`) | 235/235 |
| `asan` (ASan + UBSan) | 235/235 |
| `tsan` (includes snapshot-isolated readers racing a writer) | 235/235 |
| `release` (`-O3`; assert-only death tests compiled out) | 233/233 |
| clang 18.1 Debug, `-Werror` | 235/235 |
| `tools/check_format.sh` | OK (65 files) |
| `tools/mutation_smoke.py` | **30/30 mutants killed** (27 under `debug`, 1 under `asan`, 2 under `tsan`) |

Test design highlights: model-based table round trips for every type × row-group boundary
(0, 1, 2047, 2048, 4095, 4096, 4097, …) with random chunk sizes including empty ones;
`CanSkip` checked against brute force over random columns (soundness for every operator,
exactness for `< <= > >= <>`); pruning-soundness property test over random tables, columns,
operators and constants (every matching row must survive pruning); snapshot isolation (an old
snapshot is unchanged by 9,000 later appends; tail snapshot cached until the next append);
per-segment scan counters prove only projected columns are touched; scan vectors stay valid
after their segment/builder are destroyed (ASan) and `Reset` on them never corrupts the segment;
catalog creation race (8 threads, exactly one winner) and 4 readers scanning while a writer
appends 84,000 rows across many group seals, each reader verifying it saw a consistent prefix
(checked under TSan).

**Found by the process**
1. Mutation testing again earned its keep: the tool itself needed hardening after an interrupted
   run left a mutated `string_t.h` in the working tree and a careless `git add -A` staged it;
   the failing test caught it before anything was pushed. The tool now refuses to run on dirty
   targets, restores on SIGTERM/SIGHUP/SIGINT, keeps on-disk backups (repairing leftovers from a
   SIGKILL), validates every anchor before building, and verifies byte-for-byte restoration.
2. A scripted edit silently no-opped because clang-format had reflowed the target line
   (`Vector::FlatData<T>()` lacked its read-only assert until a death test failed to die).
   Edit helpers in this repo now assert that their anchor matched.

**Known gaps / deliberate limits**
- Segments are uncompressed (Phase 5). `Table` is append-only; no UPDATE/DELETE (Phase 9 at the
  earliest). No persistence yet (Phase 7): everything is in memory.
- Tail snapshots copy the open row group's data (≤ 122,880 rows) once per append-then-read cycle;
  fine for analytical load patterns, wasteful for interleaved single-row inserts and scans.
- String zone-map bounds are dropped (never pruned) for segments containing a string > 64 bytes.
- `BM_Table_Append` is dominated by test-data generation; it is not a pure append benchmark.

**Phase 2 exit criteria met** (append → scan round trips for all types incl. NULLs and long
strings; projection touches only requested columns, asserted; concurrent readers + writer clean
under TSan).

**CI (GitHub Actions, run 37383454174)** — all 7 jobs green on the pushed branch: format,
gcc-13 and clang-18 × debug and release, asan (ASan+UBSan), tsan.

---

## 2026-10-06 — Phase 3: SQL front end
**Branch:** `phase-3-sql-frontend`

**Changed**
- `parser/`: lexer + recursive-descent/precedence-climbing parser → AST. SELECT (DISTINCT, all join
  forms incl. USING/CROSS/comma, derived tables with column-alias lists, WHERE/GROUP BY/HAVING/
  ORDER BY NULLS FIRST|LAST/LIMIT/OFFSET), expressions (CASE, CAST/`::`, BETWEEN, IN list/subquery,
  LIKE, IS NULL, EXISTS, scalar subqueries, EXTRACT, SUBSTRING, DATE/INTERVAL literals), CREATE/DROP
  TABLE, INSERT VALUES/SELECT, COPY FROM, EXPLAIN [ANALYZE]. UNION/WITH/constraints/schema-qualified
  names are reported as NotImplemented. AST prints fully parenthesised SQL (print/parse fixpoint);
  nesting and left-deep chains are depth-bounded; `FormatErrorWithContext` renders `LINE n:` + caret.
- `types/cast`: DuckDB-compatible `CAST`, implicit widening, `CommonSuperType`, `AddInterval`
  (month clamping). `Value::ToString` for doubles is now Python-`repr`/DuckDB style.
- `planner/`: `BoundExpr`, **`EvaluateScalar` (the reference interpreter, ADR 0004)**, logical plan
  with EXPLAIN rendering, and the **binder** (scopes, coercion, constant folding, aggregation rules,
  ORDER BY resolution incl. hidden columns and aggregates introduced by ORDER BY).
- `storage`: `ColumnDefinition::not_null` (enforced), **`Table::Merge`** (atomic publish of a staging
  table). `io/`: **CSV loader** (RFC 4180, atomic via staging).
- `main/`: `Connection::Query`/`QueryAll`, `QueryResult`; executes DDL, INSERT VALUES, COPY, EXPLAIN
  and table-free SELECT. `tools/shell.cpp` → `cdb_shell`. `bench/tpch/{schema.sql,queries/}`.
- Fuzzing: deterministic mutational fuzz in the unit tests plus a libFuzzer harness
  (`cmake --preset fuzz`, `tools/run_fuzz.sh parser 60`).
- ADR 0003 (semantics: match DuckDB + documented divergences), ADR 0004.
- `tools/verify.sh` (the gate) and a hardened `tools/mutation_smoke.py`.

**Verified**
| Check | Result |
|---|---|
| `tools/verify.sh`: debug / asan / tsan / release / clang-18 | 412 / 412 / 412 / 410 / 412 passing |
| DuckDB differential (`GoldenExpressions.MatchDuckDB`) | **4,286 / 4,292 expressions identical** (type, value, error status); 6 documented divergences |
| Parser contract fuzzing (unit test: 40k mutated queries + 40k token soups + all prefixes/suffixes + raw bytes) | no violations |
| libFuzzer, `parser_fuzz`, 90 s, ASan+UBSan | 992,194 executions (~10.9k/s), no crash or contract violation |
| TPC-H binding | 12 / 22 queries bind (Q1, 3, 5–10, 12–14, 19); the other 10 fail with a positioned NotImplemented exactly on a subquery / WITH |
| `tools/mutation_smoke.py` (Phase 3 group: lexer, parser, AST printer, casts, double formatting, evaluator, binder, Table, CSV, Connection) | **28 / 28 killed.** The first run killed 26 of 28 mutants: one was a real test gap (CRLF left in a *text* column) and one was unreachable code (removed); one mutant did not compile and was repaired. Phase 1–2 mutants were not re-run this phase (their sources are unchanged); the full suite of 58 is re-run at the end of Phase 4. |

Test design highlights: parse → print → parse fixpoint over a corpus and the 22 TPC-H queries;
~40 positioned syntax-error cases plus caret rendering; every binder error path with its source
position and reviewed golden EXPLAIN plans; scalar interpreter checked against compiler overflow
builtins and an independent DP `LIKE` matcher over 30k UTF-8 cases; CSV quoting/atomicity corner
cases and a 30k-row randomized round trip; `Table::Merge` visibility under a concurrent reader.

**Found by the process**
1. The DuckDB differential test found 82 disagreements on its first run (98.1 % agreement) that
   unit tests had not: `DOUBLE→INT` rounds half-to-even, `'0x10'`/`'1_000'` parse as integers,
   `'5' + 1` must be an error, `NULLIF`/`ROUND` result types, `x % 0.0` is NaN, `-NULL` is BIGINT,
   `CAST(NULL AS DATE) AS INTEGER` is NULL, `TRUE::DOUBLE` is legal, doubles print like Python's
   `repr`. All fixed or documented (ADR 0003).
2. Fuzzing found a derived table without an alias that printed unparseable SQL.
3. The binder could not handle `ORDER BY max(a)` when that aggregate was not otherwise in the
   query; the aggregate operator is now built after ORDER BY resolution.
4. `set -e` was silently ineffective in the harness's chained shell commands, so a failing test did
   not stop a commit and an interrupted mutation run left a mutated file that was nearly committed.
   Both are fixed structurally (`tools/verify.sh`, a crash-safe mutation tool, explicit staging).

**Known gaps / deliberate limits**
- No query execution over tables yet (Phase 4). Subqueries, `WITH`, `UNION`, `FULL`/`RIGHT` + `USING`
  are NotImplemented. `DECIMAL` is `DOUBLE`; `SUM(INT)` is `BIGINT`; `UPPER`/`LOWER` are ASCII-only;
  `DATE` covers years 1–9999 (ADR 0003).
- A multi-statement script is parsed in full before any statement runs (a syntax error anywhere
  means nothing executes), but bound statement-by-statement.
- COPY loads whole files through the loader's line reader (no parallel parsing yet).

**Phase 3 exit criteria met** (parser round-trip tests, positioned error tests, parser fuzzing
clean for a fixed budget, binder + plan tests).

**CI (GitHub Actions, run 37388927631)** — all 8 jobs green on the pushed branch: format,
gcc-13 and clang-18 × debug and release, asan (ASan+UBSan), tsan, and the new libFuzzer parser
smoke job (45 s).

## 2026-10-06 — Phase 4: Vectorized execution (v0.1)

**What changed**
- `execution/expression_executor`: vector-at-a-time evaluation of bound expressions with typed,
  NULL-aware kernels over flat / constant / dictionary inputs; lazy `AND`/`OR`/`CASE`/`COALESCE`;
  `Select()` writes predicates straight into selection vectors, narrowing between conjuncts.
  Shared UTF-8 / `LIKE` / `SUBSTRING` helpers in `common/string_ops`.
- Push-based pipelines ([ADR 0005](adr/0005-push-pipelines-with-global-and-local-state.md)): operators
  with source / streaming / sink roles and global + local state; `Executor` runs a `PhysicalPlan`.
  Operators: table scan, `VALUES`, filter, projection, limit, hash aggregate (also `DISTINCT`, with
  `DISTINCT` aggregates), `ORDER BY`, top-N, hash join (inner / left / semi / anti, multi-key,
  residuals, nested-loop fallback), result collector, `INSERT` (staging + atomic merge). Building
  blocks: `ChunkStore`, `KeyIndex`, `GroupTable`, mergeable `AggregateState`s, hashing.
- Rule-based optimizer ([ADR 0006](adr/0006-rule-based-optimizer.md)): outer-join-aware filter
  pushdown, zone-map hints, greedy join ordering sized by distinct-value estimates, `OR` factoring,
  `LIMIT` below projections, column pruning. Physical planner with join-key extraction and `RIGHT`
  joins as swapped `LEFT` joins.
- `Connection` runs `SELECT` and `INSERT ... SELECT` end to end; `EXPLAIN` shows the optimized plan;
  `SetOptimizerEnabled`. Shell: result truncation, `.timer`, `.maxrows`.
- Tooling: `sqllogictest`-style runner + `tools/gen_slt.py` (DuckDB fills in expected results),
  `cdb_tpch` runner and `tools/tpch_duckdb_time.py`, TPC-H data generation in CI, 45 new mutants.

**Verified**
| Check | Result |
|---|---|
| `tools/verify.sh`: debug / asan (+UBSan) / tsan / release / clang-18 | 552 / 552 / 552 / 550 / 552 passing |
| **TPC-H vs DuckDB** (`tests/execution/tpch_test.cpp`): Q1, 3, 5, 6, 7, 8, 9, 10, 12, 13, 14, 19 | **identical at SF0.01 (in the gate and CI), SF0.1 and SF1** (rows and order; doubles within 1e-9 relative) |
| SQL suite `tests/sql/*.test` (8 files, 241 queries + 82 statements, expected results generated by DuckDB) | all pass; no engine bug found, 3 DuckDB typing differences handled in the queries (ADR 0003) |
| `ExecutorDifferential`: 8 seeds x 1,200 random typed SQL expressions x 3 random chunks (flat/constant/dictionary, 0-2048 rows, NULLs, benign + full-range data) vs `EvaluateScalar` | identical values (bitwise), identical error behaviour, `Select` = the TRUE rows |
| `OptimizerEquivalence`: 6 seeds x 600 random queries (all join kinds, derived tables, aggregates, DISTINCT, ORDER BY/LIMIT, NULL data) with the optimizer on vs off; 12 `OR` shapes; 10 reviewed plan shapes | identical results |
| Operator tests vs naive references (filter/projection chains, limit at every chunk alignment, aggregates, DISTINCT, ORDER BY, top-N across pruning, joins x 4 types x 0-2 keys x residual, 10k-row output from one probe chunk, several local sink states per operator, atomic INSERT) | all pass |
| `tools/mutation_smoke.py` (debug preset; 58 older mutants re-run for the first time since Phase 3 plus 45 new ones for the executor, operators, planner, optimizer, string helpers and lazy vector storage) | **103 / 103 killed.** The first run over the new mutants killed 37 of 47: five survivors were real test gaps (below, item 4) and five mutants did not compile under `-Werror` and were rewritten; after the fixes every one is killed. |

**Performance** (full tables, machine, build and commands in [BENCHMARKS](BENCHMARKS.md); single thread
vs DuckDB 1.5.6 pinned to one thread): geometric mean over the 12 queries **1.7x** DuckDB's time at
SF0.1 and **3.1x** at SF1. Scan/aggregate queries are within 1.2-1.9x; the multi-way joins at SF1
(Q7 10x, Q8 8x, Q9 11x) are the weak spot. SF1: lineitem (6.0 M rows) loads in 5.1 s, peak RSS 1.7 GB.

**Found by the process**
1. The executor-vs-interpreter differential found two real bugs in code that already had tests:
   the interpreter's `NULLIF(a, b)` skipped `b` for NULL `a` (Postgres/DuckDB evaluate both), and
   `"\xFFaa" LIKE '%a'` disagreed between the byte-wise fast path and the character-wise matcher
   because `0xFF` was treated as a 3-byte character. UTF-8 segmentation is now defined for every byte
   string (ASCII bytes are always character starts) and the LIKE fast paths are limited to ASCII-led
   literals. It also pinned down one deliberate difference: in selection position `a AND b` does not
   evaluate `b` where `a` is NULL (it cannot be TRUE), so an error in `b` there is raised by the
   interpreter only.
2. Running real queries found two plans that were orders of magnitude off: TPC-H Q19 took 46 s as a
   nested loop over a cross product (the join key hid inside an `OR`; fixed by factoring common
   conjuncts, 9 ms) and Q5 took 2.7 s because the greedy join order compared only relation sizes and
   joined `customer` on a 25-valued key first (fixed by sizing joins with distinct-value estimates
   from zone maps, 21 ms; a regression test locks in the shape).
3. A callgrind profile of Q9 showed `memset` at 35% of all instructions: `Vector::Reset()` allocated
   and zeroed a buffer even when the next use was `Reference()` again. Allocation is now lazy
   (join-heavy queries 20-30% faster). An attempt to leave `SelectionVector`s uninitialised gave no
   measurable gain and was reverted.
4. Designing mutants exposed what results cannot show: the sink protocol was only exercised with one
   local state (now driven with 1-4 for every breaker), planner choices (top-N, hash vs nested loop,
   build side, RIGHT-join swap, shared snapshot) had no plan-level tests, a LIMIT that ends in a later
   chunk, composite-key sizing, UTF-8 sequences cut off at the end of a view, `ToUnified` as the first
   access of a lazily allocated vector, and the `KeyComparator` NULL rule were all untested. All have
   tests now; five mutants that did not compile under `-Werror` were rewritten.
5. The DuckDB-generated SQL suite found no engine bug, but showed three typing differences DuckDB has
   and this engine (by ADR 0003) does not: `DATE + INTERVAL` is a TIMESTAMP, `2.5` is a DECIMAL (so
   `CAST(2.5 AS INTEGER)` rounds half away from zero where DOUBLE rounds half-even), and DATE has a
   wider range. The queries cast explicitly.

**Known gaps / deliberate limits**
- **Single-threaded.** The global/local protocol is tested with several local sink states, but nothing
  runs concurrently yet (TSan only ever sees one executing thread); Phase 6.
- `FULL OUTER JOIN` is NotImplemented; semi/anti joins exist in the operator but nothing generates them
  until subqueries are unnested; 10 of the 22 TPC-H queries need subqueries (Phase 8).
- The optimizer has no statistics: join order is a greedy left-deep heuristic, `LIKE` has a fixed 0.2
  selectivity (the real one for `%green%` is 5.3%, which misorders Q9), and there is no `EXPLAIN
  ANALYZE` or physical-plan `EXPLAIN` (Phase 8).
- The SF1 gap on Q7/Q8/Q9 is probably the join hash table's memory layout (a hypothesis, not measured);
  Phase 5.
- CSV loading is single-threaded (lineitem SF1: 5.1 s).
- TPC-H SF0.1 and SF1 are run by hand (`CDB_TPCH_SF=1`), not in the gate or CI (1.1 GB of data, 1.7 GB
  of memory); SF0.01 is in both.
- DECIMAL is DOUBLE, so sums differ from DuckDB in the last digits (compared with 1e-9 relative
  tolerance).

**Phase 4 exit criteria met** (Q1/Q6 and the join-heavy queries match DuckDB at SF0.1 and SF1; first
honest numbers recorded in `BENCHMARKS.md`).

**CI (GitHub Actions, run 37407175257, the last commit that changed code)** - all 8 jobs green: format,
gcc-13 and clang-18 x debug and release, asan, tsan and the libFuzzer parser smoke job; every test
job generated the TPC-H data and required the 12 differential tests (CDB_REQUIRE_TPCH=1).

## 2026-10-06 — Phase 5: Compression and SIMD kernels

**What changed**
- Per-segment encodings, chosen when a row group is sealed
  ([ADR 0007](adr/0007-segment-encodings-and-simd-dispatch.md)): constant, run-length, bit-packed
  integers (frame of reference, or delta for non-decreasing vectors), scaled doubles (`n / 10^e`,
  checked bit for bit so they are lossless, with a per-vector raw fallback) and dictionary strings
  (scans hand back DICTIONARY vectors over one shared dictionary). An encoding is used only if it is
  at most 70% of the raw size; the open tail of a table stays raw; `SetCompressionEnabled` /
  `CDB_NO_COMPRESSION` switch it off. Bit unpacking is specialised per width (compile-time
  constants, no loop).
- AVX2 kernels in `src/kernels/` with runtime CPU dispatch and a scalar fallback (`CDB_NO_SIMD`,
  per-function `target("avx2")`, never `-march=native`): compare-to-selection (int32 / int64 /
  double, NaN-as-largest and -0.0 preserved), SUM / MIN / MAX, scaled-double decode, integer hash and
  hash-combine.
- Engine integration: `Select` runs `column <op> constant` through the compare kernel; ungrouped
  SUM / MIN / MAX use the aggregate kernels (integer `SUM` only where a left-to-right checked sum
  provably cannot overflow either, so overflow errors do not depend on the CPU); hashing hashes each
  distinct dictionary entry once and short strings from their 16 inline bytes; the hash-join probe
  looks every bucket up first and prefetches (hash and chain pointer now live in one entry); decode and
  gather write into unzeroed storage (`Vector::FlatDataForOverwrite`, `SelectionVector::Uninitialized`).
- Tooling: `bench/kernel_bench.cpp` (every kernel, scalar vs AVX2), `cdb_tpch --no-compression` and
  stored size, 57 new mutants (and `--check` repaired a Phase 4 mutant whose anchor became ambiguous).

**Verified**
| Check | Result |
|---|---|
| `tools/verify.sh`: debug / asan (+UBSan) / tsan / release / clang-18 | 597 / 597 / 597 / 595 / 597 passing (Phase 4: 552 / 552 / 552 / 550 / 552) |
| Bit packing: every width 0-64 x lengths 0..2048 round trip, guard bytes after the stream untouched, high input bits ignored | pass |
| Every encoding x every data shape (constant, two values, ascending, jumps, descending, narrow around a huge base, runs, random, extremes; money, rates, integers, NaN / infinity / -0.0 / denormals, long strings) x 0 / 20% / 100% NULLs x lengths 1, 2, 63, 2047, 2048, 2049, 4096, 5000, 10000, scanned in order and out of order, plus 6 threads scanning shared segments | bit-identical to the raw values |
| Kernels vs their scalar reference: every length 0-70 (1-70 for the aggregates) plus the 127-2048 vector boundaries, special values (NaN, -0.0, `INT_MIN`, overflow edges), SIMD on and off | identical (`SUM(DOUBLE)` is re-associated: compared with a tolerance, and exactly for exactly representable inputs) |
| **Whole suite with `CDB_NO_SIMD=1`, with `CDB_NO_COMPRESSION=1`, and with both** (release; includes the DuckDB-generated SQL suite and TPC-H SF0.01) | 595 / 595 in each configuration: results do not depend on the CPU or the storage layout |
| **TPC-H vs DuckDB with compression and AVX2 on** (12 queries): SF0.01 in the gate and CI, SF0.1 and SF1 by hand (`CDB_TPCH_SF`) | identical (rows, order; doubles within 1e-9 relative) |
| `tools/mutation_smoke.py`: 57 new mutants (bit packing, every encoding, segment scan, the AVX2 select / decode / aggregate / hash kernels, executor and aggregate integration, dictionary hashing) plus the 103 older ones re-run | **160 / 160 killed.** First run of the new ones: 49 / 57 - two survivors were real test gaps and six did not compile under `-Werror` (below). |

**Performance** (tables, machine, build and commands in [BENCHMARKS](BENCHMARKS.md); GCC 13.3 `-O3`, one
thread, DuckDB 1.5.6 pinned to one thread):
- Kernels, scalar -> AVX2 or width-specialised, per row: unpack 1.4-4.3x, select 3.2-6.3x, scaled-double
  decode 4.0x, SUM 1.6-8.2x, MIN+MAX 3.9-5.4x, hash 1.9-2.3x.
- Memory, TPC-H SF1 stored columns: 1408 MB -> 612 MB (**2.3x**; lineitem 994.7 -> 337.5 MB, **2.9x**);
  peak RSS after loading 1498 -> 720 MB.
- TPC-H SF1 geometric mean against DuckDB: **3.1x -> 2.6x** (2.4x with compression off); SF0.1 1.7x,
  unchanged. Compression itself costs 0-30% on scan-bound queries (decoding versus a zero-copy scan:
  Q6 35.6 -> 44.3 ms); most of the SF1 gain comes from the join-probe prefetching and cheaper hashing
  (Q9 2488 -> 1420 ms), which also confirms the Phase 4 hypothesis that the SF1 join gap was memory latency.

**Found by the process**
1. **UBSan in the gate** caught `BitUnpack(count = 0)` calling `memset` / `memcpy` with a null pointer,
   which is undefined behaviour even for zero bytes (the debug and release builds had passed). Empty
   segments reach it, so the function now returns early; the test, which deliberately includes length 0,
   was left as it was.
2. **Mutants found two real test gaps:** nothing exercised scaled-double offsets of 53-54 bits (the
   "OR into the mantissa of 2^52" conversion is only exact below 2^52), and nothing pinned the exact
   dictionary limit (2,047 distinct strings, so a NULL entry always fits). Both have tests now. Six
   more mutants did not compile (an unused variable or parameter, a shift by 64) and were rewritten as
   equivalent bugs that do.
3. **Measuring deleted or rewrote code:** an AVX2 integer decode was slower (110 vs 83 ps/row) than the
   loop the compiler already vectorises and was removed; the first AVX2 MIN/MAX was 3x *slower* than
   scalar because the accumulators were kept in stack slots, and 5.4x faster once they stayed in
   registers.
4. **SIMD must not change SQL-visible behaviour.** A vector `SUM(BIGINT)` adds in four lanes, so it can
   overflow where the sequential sum does not (`M, -M, 0, 0, ...`). The kernel is conservative, the
   ungrouped fast path is taken only when `max|x| * n` is provably in range, and a test pins the
   case; `SUM(DOUBLE)` is the one deliberately re-associated reduction.
5. Two lessons about the tooling. `--check` found that adding a second `if (group_types_.empty())`
   to `GroupTable::Sink` made an older anchor ambiguous (it is now pinned). And building another
   preset while the mutation script runs compiles the *mutant* (it edits `src/` in place): I did
   exactly that and got false `StringT` / `ExecutorDifferential` failures; the rule is in CLAUDE.md.

**Known gaps / deliberate limits**
- **Predicates are not evaluated on compressed data.** Scans decode, then compare; compression pays
  in memory and in hashing, not yet in filtering (RLE / dictionary-code predicates, FSST, ALP
  exceptions and sorted dictionaries are not implemented). The roadmap's SIMD string-prefix compare
  was not done either; string predicates are unchanged.
- Decoding costs 0-30% on scan-bound queries (above); `CDB_NO_COMPRESSION` is the escape hatch.
- Only AVX2 + scalar. CI runs x86-64 machines with AVX2, so the scalar path is tested by forcing it
  (`CDB_NO_SIMD`, whole suite green above), not on a CPU without AVX2; there is no AVX-512 or NEON path.
- The open tail of a table (the unsealed last row group) is never compressed.
- SF1 multi-way joins are still 6.3-8.4x DuckDB (Q7, Q8, Q9): row-wise payload storage on the build
  side and a `LIKE` selectivity estimate are the next steps (a hypothesis, not yet measured).
- Still single-threaded (Phase 6), including CSV loading.
- `SUM(DOUBLE)` over a flat vector re-associates the additions, so its last bits can differ from a
  left-to-right sum (deterministic for a given input).

**Phase 5 exit criteria met**: round-trip property tests for every encoding; before/after numbers for
each kernel in `BENCHMARKS.md`; memory footprint measured on TPC-H lineitem (2.9x).

**CI (GitHub Actions, run 37421340867, the last commit that changed code)** - all 8 jobs green: format,
gcc-13 and clang-18 x debug and release, asan, tsan and the libFuzzer parser smoke job; every test
job generated the TPC-H data and required the 12 differential tests (CDB_REQUIRE_TPCH=1).


## 2026-10-06 — Phase 6: Morsel-driven parallelism

**What changed** ([ADR 0008](adr/0008-morsel-driven-parallelism.md))
- `TaskScheduler`: a fixed pool where a job is `body(participant)`; the caller is participant 0, so nested
  and concurrent jobs cannot deadlock; the first exception is rethrown after every participant returned.
  `Database` owns it (`CDB_THREADS`, `SetThreads`; library default **1 thread**, the shell and `cdb_tpch`
  default to every hardware thread).
- Pipelines run on several threads when the source, every operator and the sink say so; `LIMIT` and any
  operator not audited stay serial. A table scan is a `MorselScan`: morsels of up to 8 vectors (never across a row
  group, zone-map pruning unchanged) claimed from an atomic cursor, and sized by the thread count for small tables.
- Result order: chunks carry a batch index (morsel number), so `SELECT ... WHERE` and `INSERT ... SELECT` keep table
  order on any thread count.
- Aggregate: per-thread tables, merged by hash partition from 32,768 groups (each partition by one task, no locks),
  result served as a parallel source. **Integer `SUM` is now exact (128-bit accumulator, range-checked when the result
  is produced)**, so the value or the overflow error depends on the data only, not on thread count or SIMD lanes - a
  behaviour change for the serial engine too (ADR 0003), forced by parallelism and the one DuckDB has.
- Join build: stores handed over, chunks adopted whole, hashed in parallel, linked into chains by bucket partition
  without atomics. Sort: parallel stable merge sort with co-rank slices, identical to the serial stable order; top-N
  prunes per thread. `COPY`: mmap, parallel record index, whole row groups parsed / sealed / compressed per task, same
  tables and same errors (line numbers included) as the serial loader.
- Tooling: `-parallel` test presets (4 threads, one-vector morsels, every threshold at 1) so the *whole* suite also runs
  in the interleaving-heavy configuration under debug, release, ASan and TSan; `verify.sh` and both CI test jobs run
  both modes. `cdb_tpch --threads N --sql "..."`, shell `.threads N|auto`.

**Verified**
| Check | Result |
|---|---|
| `tools/verify.sh`: debug / asan (+UBSan) / tsan / release / clang-18, each configuration run in default **and** `-parallel` mode | 671 / 671 / 671 / 669 / 671 passing (Phase 5: 597 / 597 / 597 / 595 / 597); both modes green under all four primary presets |
| ThreadSanitizer, both modes (incl. 4 sessions sharing one pool, 8-thread joins/sorts/aggregates, parallel CSV, nested jobs) | clean |
| Results vs one thread: 12 SQL queries (grouped and ungrouped aggregates, DISTINCT, joins of every type, sorts, top-N) on 102,400 rows with 1 vs 4 threads, a 12,800-row table with 1 vs 8, operator-level tests on 1/2/4/8 threads against naive references | identical (floating-point sums over exactly representable values; ties only where the order is defined) |
| Parallel merge sort vs serial `stable_sort` over 7 key shapes (3 with no unique key, so stability is observable), 0-40,000 rows, 2-16 threads | identical order |
| Parallel CSV vs serial loader: tables, row groups, every error message and line number, quoted multi-line records, CRLF, blank lines, headers, stray quotes; offline differential fuzz of 300,000 random inputs | 0 mismatches |
| TPC-H vs DuckDB, 12 queries, SF0.01 in the gate (both modes) and SF0.1 / SF1 by hand | identical |
| `tools/mutation_smoke.py`: 52 new Phase 6 mutants (scheduler, morsels, executor, ordering, aggregate / join / sort merges, CSV, adaptive morsels) and the 9 older ones whose code changed | all killed (first run of the first 55: 51 killed, one real test gap and three that did not compile, below; the 6 adaptive-morsel mutants and a repaired anchor: 11 / 11) |

**Performance** (full tables, commands and caveats in [BENCHMARKS](BENCHMARKS.md); Ryzen 7 6800H 8C/16T, GCC 13.3 `-O3`,
min of 5)
- TPC-H SF1 speedup over one thread at **16 threads: geometric mean 5.5x** (Q1 6.4x, Q3 4.6x, Q6 5.8x, Q12 6.5x, Q13 6.6x,
  Q19 6.6x; worst Q10 4.0x, Q7 4.2x); at 8 threads Q1 5.3x. DuckDB on the same machine: 3.7x geometric mean (Q1 6.6x, Q6 4.9x).
  At 16 threads the SF1 geometric mean against DuckDB is **1.7x**, against 2.6x on one thread. SF0.1: 3.9x.
- **The roadmap target of >= 8x at 16 threads is not met.** The machine has 8 physical cores; 8 -> 16 threads helps some
  queries and hurts others (Q7, Q10).
- Operators: parallel `ORDER BY` of 6 M rows 7.5x, 1.5 M-group aggregation 5.1x, a 6 M-row join 6.1x, `COPY` of lineitem
  5.5x at 8 threads (6.9 s -> 1.25 s). `count(DISTINCT)` does **not** scale (1.3x).

**Found by the process**
1. **Measuring found a scaling bug:** Q13 at SF0.1 did not scale at all (46 ms on 1 thread, 28-44 ms on 2-16). `customer`'s
   15,000 rows were a single 16,384-row morsel, so the probe and the group-by after it ran on one thread. Scans now size
   their morsels by the thread count (50.2 -> 9.8 ms at 16 threads); tests pin the rule, the executor's hint and the
   pruned-rows arithmetic. DuckDB has the same plateau at this size.
2. **Mutants found a real test gap:** `std::stable_sort` -> `std::sort` for the parallel sort's runs survived because every
   key shape of the test ended in the unique `id` column, so no two rows ever tied and stability could not be observed. Three
   shapes without a total order now run in the same test. Three more mutants did not build under `-Werror` (unused
   variable / parameter) and were rewritten as `(void)x, false` equivalents.
3. **Fuzzing found a silent-data-loss bug** in the parallel CSV loader (an unterminated header record was dropped without
   an error); the header record is now checked with `SplitRecord` first. A related find: the *serial* loader re-split a
   multi-line record from its start on every line, which made a stray quote quadratic (a 541-second test); an incremental
   `QuoteScanner` replaced it, with a debug assertion that it agrees with `SplitRecord`.
4. **Parallelism changed a specification.** An integer `SUM` as a running 64-bit total fails or succeeds depending on which
   thread saw which values (`{MAX, 1, -1}`). The old tests had encoded the order-dependent rule; they were rewritten in a
   separate commit with the reason, and strengthened (exact in 128 bits, error only when the final total leaves BIGINT).
5. A **death test** caught that the parallel-loading change had dropped the column-shape check in `Table::Append` for empty
   chunks; restored. `-Woverloaded-virtual` rejected a `Finalize` overload (renamed `FinalizeParallel`).
6. **Process:** the mutation script rewrites `src/` in place, and building anything while it runs compiles the mutant. The
   Phase 5 rule (build nothing meanwhile) is why the full mutation regression now runs in its own `git worktree`, so work
   and measurement can go on in the main tree.

**Known gaps / deliberate limits**
- Speedup at 16 threads is 4.0-6.6x on SF1 (target was >= 8x, not met); the causes for Q7 / Q10 / SF0.1 are not profiled yet.
- **`DISTINCT` aggregates do not use the partitioned merge** and do not scale (`count(DISTINCT x)` 1.3x, slower at 16 threads
  than at 8): per-thread (group, value) tables are merged whole on one thread.
- Not defined with more than one thread: the order of groups, of matches within one probe row, and of ties in a sort
  without a total key (the library default of one thread keeps the old deterministic behaviour). Floating-point `SUM` /
  `AVG` re-associate across threads and SIMD lanes (equal up to rounding, not reproducible run to run).
- Parallel `COPY` needs a regular file under 4 GiB with records under 4 GB; anything else takes the serial path. Peak RSS
  while loading grows with the thread count (720 MB at 1 thread, 1266 MB at 16, SF1).
- Morsel sizing adapts only table scans; sorted-buffer and aggregate-result sources use fixed ranges. No work stealing, no
  NUMA awareness, one pool per `Database`.
- The single-thread sort (5.5 s for 6 M rows with three keys) is slow in absolute terms (index array + row comparator);
  a normalised-key sort is future work.

**Phase 6 exit criteria met**: TSan clean (both modes, locally and in CI); scaling curve for 1-16 threads for Q1 / Q3 / Q6
(and the other nine queries) in `BENCHMARKS.md`; results identical to single-threaded (tolerance only for floating-point
sums). The target of >= 8x at 16 threads was not met (above).

**CI (GitHub Actions, run 37443156698)** - all 8 jobs green, now including the `-parallel` test steps: format, gcc-13 and
clang-18 x debug and release, asan, tsan and the libFuzzer parser smoke job. The run before it (37442929725) ran 0 jobs
because a step name containing `: ` made the workflow invalid YAML; `verify.sh` now parses the workflow. The regression
run of the *older* mutants after the Phase 6 changes runs in a separate worktree and is recorded with the Phase 7 entry.

## 2026-10-06 — Phase 7: Persistence

**What changed** ([ADR 0009](adr/0009-persistence.md))
- A database is a directory: `checkpoint-<epoch>.cdb` (header, one CRC-32C-checked block per column segment stored *in its
  in-memory encoding*, footer directory, fixed trailer), `wal-<epoch>.log` (frames `[len][crc][seq][flags][payload]`, a commit
  flag on the last frame of a transaction) and a `LOCK`. `Database(path, options)` opens it; `CHECKPOINT` is a SQL statement;
  the shell takes `--db DIR`.
- Commit protocol: validate and stage (INSERT ... SELECT and `COPY` already build private staging tables), log, fsync, apply -
  under one commit mutex, so log order is apply order. A failed write or fsync **poisons** the log: nothing was applied, the
  statement fails, the database is read-only until reopened. `SyncMode::Off` trades the last statements for speed, never order.
- Checkpoint: snapshot every table and rotate the log under the mutex, write the file *outside* it (commits continue into the
  new log), rename, fsync the directory, delete the old files. Recovery: newest checkpoint (**corrupt = error, never a silent
  fallback to an older one**) + the contiguous chain of logs; a torn tail is cut at the last committed frame; stale files and
  `*.tmp` are removed; every step is idempotent, so a crash during recovery is just another crash.
- All I/O goes through `FileSystem`. `PosixFileSystem` is the real one; `MemoryFileSystem` has a **crash model** (per-file durable
  image plus unsynced operations; directory entries are durable only after a directory fsync; `Crash(policy)` drops everything
  unsynced, keeps everything, or keeps a random prefix with the last write torn at a random byte); `FaultInjector` makes the Nth
  operation crash or fail.
- Decoders validate what a checksum cannot: bit widths, offsets inside the payload, increasing run ends, dictionary codes, row
  counts, and a guard against a tiny file asking for a huge allocation. Loading is parallel (a task per row group); CRC-32C uses
  the SSE4.2 instruction with a table fallback (equal results tested).

**Verified**
| Check | Result |
|---|---|
| `tools/verify.sh`: debug / asan (+UBSan) / tsan / release / clang-18, each in default **and** `-parallel` mode | 835 / 835 / 835 / 833 / 835 passing (Phase 6: 671 / 671 / 671 / 669 / 671) |
| Crash campaign: a scripted workload (several tables and types, NULLs, long and multi-frame values, DROP and re-CREATE, INSERT ... SELECT, `COPY`, `CHECKPOINT`, a tiny automatic-checkpoint threshold) crashed at **every** I/O operation under every crash policy and several seeds, then recovered and compared with an in-memory reference; then crashed again *during recovery* at every operation | state is always the state after the last acknowledged statement, or the one in flight (all or nothing) - never anything else |
| I/O errors injected at every operation | the statement fails, nothing half-applied, the database refuses further writes, a reopen gives a committed prefix |
| Corruption: every single-byte flip and every truncation of a valid checkpoint; every flip / truncation of a log | checkpoint: always an error; log: always a committed prefix, never garbage |
| libFuzzer targets reading arbitrary bytes as a checkpoint and as a log (seeded from valid files) | no crash, no UB, only `Error`: 660,000 / 360,000 executions |
| Real `kill -9` of a writer process on a real disk at random moments | every acknowledged row survived, in order |
| SQL logic suite (DuckDB-verified) on a persistent database with a simulated power cut and recovery after **every statement**; TPC-H vs DuckDB on databases reopened from a checkpoint and from a log alone | identical |
| Concurrent sessions committing while checkpoints run, then reopen (also under TSan) | clean |
| `tools/mutation_smoke.py`: 42 new persistence mutants (log, commit, checkpoint, recovery, file formats, the memory file system's crash model) | all 42 killed (first run 40: one survivor and one that did not build, below; both killed / valid after the fixes) |

**Performance** (tables, commands and caveats in [BENCHMARKS](BENCHMARKS.md); NVMe, ext4, GCC 13.3 `-O3`)
- Commit latency of a single-row `INSERT`: **~0.5 ms with fsync** (1,830-1,910 statements/s, p99 0.7 ms), 40 us with
  `SyncMode::Off`, 26 us in memory. Writing and checksumming a frame costs ~14 us; the rest is the disk.
- TPC-H SF1 (8.66 M rows): reopening from a 504 MB checkpoint takes **0.16 s at 16 threads (0.31 s at one)**, 16x / 28x faster than
  loading the CSV, because the file stores the already-encoded segments. Replaying the 1,041 MB log of the same data takes
  ~3 s. A logged `COPY` costs 0.7 s more than an unlogged one at 16 threads.

**Found by the process**
1. **A mutant survived and exposed a crash-ordering hole that single-session tests cannot see.** Skipping the directory fsync after
   the new log is created is invisible to a lone session (its commits go to the old log, which is intact). It matters when a
   second session commits *into the new log* before the checkpoint finishes: the commit is fsynced to a file whose name was not
   yet durable, so a crash loses an acknowledged commit. A new crash test with concurrent sessions kills the mutant.
2. The checkpoint reader trusted a raw segment's declared row count when allocating; a tiny file could ask for a large buffer.
   Now bounded by the bytes that remain.
3. The `kill -9` test was flaky on a loaded machine (it warmed the child up by wall time); it now waits for 100 acknowledged rows.
4. Several first-draft test expectations (operation counts, frame counts, number of checkpoints) were wrong about the *test*, not the
   code, and were corrected after reading the trace; a `Run` helper silently hid `testing::Test::Run`.
5. One asan mutant did not build under `-Werror` (unused parameters) and was rewritten.
6. **Process:** the mutation harness had no timeout. A mutant that loses a join's hash keys turns a test into an effectively
   endless nested loop and the run sat there for hours. `ctest --timeout 600` is now part of every mutant run; a timeout counts
   as a kill.

**Known gaps / deliberate limits**
- **No group commit:** concurrent sessions each pay their own fsync (~0.5 ms); throughput does not scale with sessions.
- Everything is loaded at open (no paging): open time and memory are proportional to the database. A bulk `COPY` is written twice
  (raw rows in the log, encoded segments in the next checkpoint); replaying a long log is the slow recovery path (3 s for
  SF1) and is bounded by checkpointing. Checkpoint writing does not use more threads usefully (it is bound by writing 504 MB).
- One writer at a time; readers are never blocked. No `UPDATE` / `DELETE`, so the log has three operations only.
- `kill -9` and the in-memory crash model test everything *above* the page cache; a real power cut (disk reordering, lying fsync)
  is modelled, not exercised. POSIX only (no Windows file system). No encryption, no log compression, no single-file format, no
  incremental checkpoints. An automatic checkpoint that fails is recorded, not raised (the triggering statement did commit).

**Phase 7 exit criteria met**: the crash campaign (every write / fsync / rename / directory-fsync boundary, every policy) never
loses an acknowledged commit and never shows a partial one; the format fuzz targets are clean.

**CI (GitHub Actions, run 37455419463)** - all jobs green (format, gcc-13 and clang-18 x debug and release, asan, tsan, the libFuzzer
parser smoke job), on the commit that added the Phase 7 benchmarks. A later commit repaired the one asan mutant and touched no
engine code; the regression of the *older* mutants (Phases 1-6) is re-run in full with the Phase 8 entry.

---

## 2026-10-07 — Phase 8: Subqueries, statistics, and a cost-based optimizer

**What changed** ([ADR 0010](adr/0010-statistics-subqueries-and-cost-based-joins.md))
- **Subqueries and `WITH`** (8a). `WITH` (non-recursive) is inlined through a scope chain. Subqueries are unnested while binding, so
  no subquery exists at run time: `[NOT] EXISTS` and `[NOT] IN` (AND-ed `WHERE` conjuncts) become semi / anti joins, `NOT IN` a
  **null-aware** anti join (three-valued logic), an uncorrelated scalar a cross join with a `ScalarGuard`, a correlated scalar
  aggregate a LEFT join with the aggregate grouped by the correlation keys and its empty-group default (the "count bug"). Shapes
  outside that table fail with `NotImplemented` at the offending expression. All 22 TPC-H queries run (10 more than in Phase 7).
- **Statistics** (8b). A HyperLogLog sketch (4,096 one-byte registers) per sealed column segment, computed from the raw values at seal
  time and stored in the checkpoint (format version 2, sparse or dense); `Table::Statistics()` merges zone maps and sketches, cached by
  table version. A small `Table::Merge` continues the open row group instead of sealing a short one per statement.
- **Cost-based optimizer** (8c). A cardinality estimator (equality `1/distinct`, ranges on a grid of `distinct` points, bounds
  combined as an interval, `OR` by inclusion-exclusion, joins by containment, semi / anti by key-domain coverage, aggregates by group
  distinct counts, structural distinct counts for expressions such as date parts and `CASE` of constants); join ordering by dynamic
  programming over subsets with bushy trees (<= 12 relations, greedy left-deep to 60, as written beyond), cost = rows out + 2 x
  rows built + rows probed, the smaller input builds; rules for semi / anti joins (they sink below an inner join only if they shrink
  their input, and never below a join with a one-row input; a filter commutes past them). `EXPLAIN` shows `(~N rows)`;
  **`EXPLAIN ANALYZE`** runs the query and prints estimated and actual rows, build rows and CPU time per operator (relaxed
  atomics, a null-pointer check when off). `.stats` in the shell.
- **Deterministic floating-point `SUM` / `AVG`**: compensated (double-double) accumulation, rounded once, so the result does not depend
  on thread count or morsel order (found by TPC-H Q15, below).
- **Tooling**: `tools/fuzz_sql.py` (random schemas and queries with DuckDB's answers, run with the optimizer on and off),
  `tools/stats_accuracy.py`, `tools/optimizer_ablation.py`, `tools/bench_table.py`; the gate generates and runs 1,200 random queries.

**Verified**
| Check | Result |
|---|---|
| `tools/verify.sh`: debug / asan (+UBSan) / tsan / release / clang-18, each in default **and** `-parallel` mode | 976 / 976 / 976 / 974 / 976 passing (Phase 7: 835 / 835 / 835 / 833 / 835); the last run is on the commit with the mutation-survivor tests |
| The whole suite on the release build with `CDB_NO_SIMD=1` and with `CDB_NO_COMPRESSION=1` | 968 passing each (1 skipped: the fuzz-seed writer) |
| All 22 TPC-H queries vs DuckDB | identical at SF0.01 (gate: memory, checkpoint, log), SF0.1 and SF1 (66 tests each, `CDB_TPCH_SF=N CDB_REQUIRE_TPCH=1`) |
| SQL logic files `subqueries.test` (105 queries, 18 statements) and `ctes.test` (27, 11), expected results generated by DuckDB | identical, in memory and across a simulated power cut after every statement; `NOT IN` operators checked against a three-valued reference |
| Random SQL vs DuckDB (every join kind, aggregates, derived tables, CTEs, every subquery shape, interleaved inserts), each query with the optimizer on and off | the gate's 1,200 queries; manual campaigns of 60,000 queries x 2 modes during the phase, and **18,000 fresh queries x 2 modes (seeds 600-659, 0 `NotImplemented`) on the final build**: all identical |
| Join search vs an explicit enumeration of every tree on random graphs; estimator vs true counts on random tables; optimizer on vs off on thousands of random queries with subquery conjuncts | pass |
| HyperLogLog: accuracy by magnitude and across the linear-counting switch, merge = sketch of the union (commutative, associative, idempotent), registers validated, per-type hashing (NULL, -0.0, NaN, inline vs heap strings), checkpoint round trips and corrupt sketches | pass |
| Crash campaign and corruption sweeps of Phase 7 with checkpoint format 2 | pass |
| `tools/mutation_smoke.py`: all 319 mutants (66 new in this phase) | the full run (316 mutants, one worktree, ~10 h) killed 309 and left 7 alive; all 7 are dealt with below (6 killed by new tests, 1 equivalent and replaced), and every mutant of the final script is killed |
| Distinct counts, 61 columns of TPC-H SF1 against DuckDB's exact counts | median error 0.02%, max 4.0%; the 35 columns with >= 1,000 values: median 0.52%, max 3.2% |
| Estimated vs actual rows, 293 operators of the 22 queries at SF1 | median q-error 1.01, 90th percentile 15, joins 1.14 / 10; the worst (35,000x) is Q18's `HAVING sum(...) > 300` |

**Performance** (tables, commands and caveats in [BENCHMARKS](BENCHMARKS.md); same-sitting runs, GCC 13.3 `-O3`, 8C/16T)
- TPC-H SF1, all 22 queries, min of 5: **geometric mean 2.31x DuckDB's time on one thread, 1.51x at 16 threads**; 4.4x speedup from
  1 to 16 threads (the 12 queries of the earlier phases: 5.9x, 2.00x and 1.25x). SF0.1: 3.0x, 1.29x, 0.53x. Q18, Q19, Q14 are on par with
  DuckDB on one thread; Q17 is 10x slower (below).
- Against the same engine with Phase 7's rule-based join order (the first commit of this phase): Q7 3.9x, Q18 3.3x, Q8 2.4x, Q11 2.2x,
  Q2 2.0x faster on one thread; Q1 is 31% slower (compensated sums). Optimizer off (SF0.01): 10 of the 22 queries do not finish in 45 s,
  the others are 2x to 27,000x slower.
- Cost: loading `lineitem` SF1 +7% on one thread (+4% at 16) for the sketches, checkpoint +2 MB (505.9 MB), `EXPLAIN ANALYZE` <= ~5%.

**Found by the process** (each fixed, with a test that fails without the fix)
1. **TPC-H Q15 returned no row at 16 threads on SF1.** A CTE is inlined per reference, so `total_revenue = (SELECT max(total_revenue)
   ...)` evaluates the aggregate twice, and a parallel double sum differs in its last bits between two evaluations (4,261 of 10,000
   supplier sums at 4 threads). Fixed at the root: `SUM` / `AVG` of doubles are now order-independent (+23% to +31% on Q1).
2. **The random-query fuzzer found a crash in the new join-tree rebuild**: a conjunct that mentions no column hit a
   `CDB_CHECK`. Constant conjuncts that are TRUE are dropped, others applied on the top join.
3. **The brute-force oracle for the join search found cross products inside connected plans** (a disconnected subset joined by cross
   product although a predicate could have joined it); fixed in the DP's split rule.
4. **`EXPLAIN ANALYZE` found the estimator's holes**: `qty > 49` over 1..50 estimated at nothing (a continuous range model; now a grid),
   Q5's date range multiplied as two independent bounds (now an interval), group-by expressions estimated 100x too high (date parts,
   `CASE`), and the Q18 / Q21 plan gaps (a semi join applied after the join of everything instead of on its relation).
5. **Reading the code found a wrong result**: a correlated `EXISTS` with an aggregate select list unnested to a plain semi join, which
   ignores that an ungrouped aggregate always returns a row. It is `NotImplemented` now.
6. **The HyperLogLog switch was biased**: switching from linear counting on the raw estimate (2.5 m) used the biased harmonic mean around
   10,000 values: `l_suppkey` (10,000 exact) was +4.6%. Linear counting now runs while its own estimate is <= 2.7 m (+0.5%).
7. **The benchmark found a 30% regression on Q22** (73 -> 94 ms): the NOT EXISTS over `orders` was estimated at 0 rows (it removes a
   third), so it sank below the join with the scalar subquery and probed 42,000 customers instead of 19,000. Two fixes: the semi / anti
   estimate is measured against the key's domain (est 0 -> 8,376, actual 6,384), and a semi / anti join stays above a join with a
   one-row input. Back to 74.5 ms (baseline 75.5).
8. **UBSan found a latent bug in the parallel hash join build** (CI and the asan gate; the code is from Phase 6). A build of a few rows
   has 16 buckets; with 16 threads the build has 64 partitions, and the bucket-to-partition shift was `bucket_bits - partition_bits`,
   unsigned and negative. x86 shifts modulo 64, so the join was right by accident and only a sanitizer could see it. It needs a forced
   parallel build of a handful of rows - exactly a scalar subquery's one-row build. Fixed (clamped at 0), with a test over builds of
   1 to 40 rows on 2 to 16 threads, and an asan mutant. **Process note: I pushed two commits before running the complete gate; CI's asan
   job caught what the gate would have.**
9. **The full mutation run found seven gaps in the tests** (309 of 316 mutants killed). Six were Phase 7 file-format validation
   mutants: a checkpoint with bytes between its footer and its trailer (the trailer is read from the end of the file, every checksum
   stays right), a chunk with an oversized row count (the old test supplied the count and nothing else, so the reader ran out of
   bytes first), a NULL count that the validity bits contradict (encoded and raw), RLE run ends that go back or repeat, and a dictionary
   code past the dictionary. The byte-flipping tests check that damage is *safe*, not that each validation exists; new tests
   craft inputs that are valid in every other respect. The seventh was this phase's one-row rule (the test never reached the left input
   of the join; mutants and tests now cover both). One more survivor, 'a row group of zero rows is accepted', is an **equivalent
   mutant** (a group has a column, and the segment reader rejects a segment of zero rows, so the two checks cannot be told apart); it is
   replaced by the check that is not redundant (a group larger than the table's, with valid segments). All killed on re-run.
10. Test and tool problems, not engine bugs: `std::regex` under GCC `-Werror` (hand-written parsers instead); crash-campaign breadth guards
   that counted points visited and fell when row groups became fewer (now operations covered); mutants that did not build under
   `-Werror` (rewritten with `|| true` / `&& false`); fuzz comparisons that needed DuckDB's types (DECIMAL literals, `DATE + INTERVAL`,
   `avg(INTEGER)` to 1e-12), a time cap on heavy queries, and a hash-based date-column NDV bias of -3% that is a property of the hash on
   consecutive integers (a Python replica gives -2.8%), not of the estimator.

**Known gaps / deliberate limits**
- **No semi-join reduction of decorrelated aggregates**: the inner table is aggregated whole before the join with the outer rows that
  survive (Q17: `lineitem` by `l_partkey`). DuckDB reduces it first (checked in its `EXPLAIN`). Q17 is 10x DuckDB's time on one thread,
  Q20 4.7x, Q2 3.2x. The largest single gap; first on the list for the next optimizer work.
- **No reverse semi / anti join** (build the small outer side, probe with the large subquery side): Q4, Q21, Q22 build `lineitem` / `orders`.
- Estimates: no histograms or most-common-values; a range against an aggregate's value is estimated at a third (Q18 `HAVING`, 35,000x);
  a semi / anti join with a residual predicate is estimated at 0 (Q21, 4,141 actual); the join cost does not model the cost of a probe by
  the size of the table probed (hence the one-row rule); no outer-join elimination.
- Unsupported shapes (`NotImplemented`): correlated `NOT IN`, correlated non-aggregate scalars, `IN` / `EXISTS` under `OR` or `NOT`,
  correlation two levels up, non-equality correlation of a scalar, recursive `WITH`. A CTE is inlined per reference (evaluated once per use).
- `INTERVAL` arithmetic is still constant-date only; no `UPDATE` / `DELETE`; hash tables are all in memory (no spilling).
- The full mutation run takes ~10 hours on this machine (316 sequential rebuild-and-test cycles); it is the slowest step of a phase.
- Q1 costs +31% for compensated sums; Q16 (+14%) and Q17 (+7%) are slower than the rule-based baseline and were not investigated.

**Phase 8 exit criteria met**: all 22 TPC-H queries match DuckDB at SF0.01, SF0.1 and SF1; the optimizer on / off ablation is recorded.

**CI (GitHub Actions, run 37662369969)** - all jobs green (format, gcc-13 and clang-18 x debug and release, asan, tsan, the libFuzzer
smoke job) on `c57bda8`, the last commit that touches code or tests; the docs commit after it changes neither. The two earlier pushes
that carried the Q22 fix and the HyperLogLog change failed the asan job on finding 8 above (the UBSan shift in the hash join build);
`f6cff57` fixed it (run 37584004576, green).
