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
