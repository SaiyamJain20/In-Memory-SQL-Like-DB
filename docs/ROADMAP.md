# Roadmap

A from-scratch, in-memory-first, **columnar, vectorized SQL analytics engine** in C++20 — the
architecture of DuckDB / ClickHouse / Velox, at a size one person can understand end to end.

This file is the contract: what we are building, in what order, and what "done" means for each
step. Status is updated only when a phase's exit criteria are met **and verified** (tests green,
progress logged in [`PROGRESS.md`](PROGRESS.md)).

## Project-level success criteria

These are *targets*, not claims. Anything quoted on a resume or in the README must come from a
measured result recorded in [`BENCHMARKS.md`](BENCHMARKS.md) (created in Phase 4).

| Dimension | Target |
|---|---|
| **Correct** | All 22 TPC-H queries return the same results as DuckDB at SF1 (floating point within tolerance). Differential fuzzing against DuckDB finds no unexplained mismatches. |
| **Fast** | Single-threaded TPC-H within ~3× of DuckDB; ≥ 8× speedup at 16 threads on scan/aggregate-heavy queries. We report the honest ratio even if we miss. |
| **Robust** | ASan/UBSan/TSan clean in CI. Parser and storage readers fuzzed. Crash-injection tests on persistence. |
| **Understood** | Every major design decision has an ADR in [`adr/`](adr/). Architecture doc matches the code. |

## Phases

Status legend: ⬜ not started · 🟨 in progress · ✅ done (exit criteria verified)

| # | Phase | Status | Est. |
|---|---|---|---|
| 0 | Foundation: build, CI, tests, docs | 🟨 | 1–2 d |
| 1 | Core data model: types, vectors, chunks | ⬜ | ~1 wk |
| 2 | Columnar storage and catalog | ⬜ | ~1 wk |
| 3 | SQL front end: lexer, parser, binder, logical plan | ⬜ | ~1 wk |
| 4 | Vectorized execution: expressions, operators, pipelines (**v0.1**) | ⬜ | ~1.5 wk |
| 5 | Compression, zone maps, SIMD kernels | ⬜ | ~1.5 wk |
| 6 | Morsel-driven parallelism | ⬜ | ~1 wk |
| 7 | Persistence: on-disk format, WAL, checkpoints | ⬜ | ~1 wk |
| 8 | Optimizer, statistics, subqueries, `EXPLAIN ANALYZE` | ⬜ | ~1.5 wk |
| 9 | Stretch (pick by time): spill-to-disk, window functions, pg-wire server, Parquet reader | ⬜ | 2–3 wk |

Estimates assume an agent working near-continuously; there is no human review gate between
phases — the gate is the phase's exit criteria plus green CI. ±50%.
Actuals are recorded in [`PROGRESS.md`](PROGRESS.md).

---

### Phase 0 — Foundation
CMake (C++20, GCC 13 / Clang 18), GoogleTest + Google Benchmark via FetchContent, warnings,
sanitizer presets, `.clang-format`, GitHub Actions CI, docs skeleton, and the legacy row-store
prototype retired (it lives on in git history at `a254830`).

**Exit:** `cmake --preset` configure/build/test works locally; CI green on gcc + clang +
ASan/UBSan; docs merged.

### Phase 1 — Core data model
`LogicalType` (BOOLEAN, INTEGER, BIGINT, DOUBLE, DATE, VARCHAR), `Value`, validity bitmask,
`SelectionVector`, `Vector` with FLAT / CONSTANT / DICTIONARY formats, 16-byte string views with
inline short strings and a shared string heap, `DataChunk` (vector size 2048), 64-byte-aligned
buffers, arena allocator.

**Exit:** unit tests for every format × type × null combination, including slicing, flattening and
dictionary-of-dictionary; sanitizer clean; micro-benchmarks recorded.

### Phase 2 — Columnar storage and catalog
`ColumnSegment`, `RowGroup` (~120k rows), `Table` with append and projected scan, per-segment
zone maps (min / max / null count), `Catalog`, `Database` / `Connection` API.

**Exit:** append → scan round-trips for all types incl. nulls and long strings; scan with
projection touches only requested columns (asserted by test); concurrent readers + one writer safe
(TSan).

### Phase 3 — SQL front end
Hand-written lexer and recursive-descent / Pratt parser → AST → binder (name resolution, type
inference and coercion, function resolution) → logical plan. DDL/DML: `CREATE TABLE`,
`INSERT … VALUES`, `COPY … FROM` (CSV). Queries: projections, expressions, `WHERE`, `GROUP BY`,
`HAVING`, `ORDER BY`, `LIMIT`, joins, `CASE`, `LIKE`, `IN`, `BETWEEN`, `CAST`, dates / intervals.

**Exit:** parser round-trip tests, error-message tests with source positions, parser fuzz target
running clean for a fixed budget.

### Phase 4 — Vectorized execution (v0.1)
Expression executor with typed, null- and selection-vector-aware kernels; push-based pipelines
(source → streaming operators → sink) with global/local sink state; operators: scan, filter,
projection, hash aggregate, hash join (inner/left/semi/anti), sort, top-N, limit. Interactive
shell. `sqllogictest`-style runner. Differential test harness against DuckDB. TPC-H data via
DuckDB's generator.

**Exit:** TPC-H Q1 and Q6 (plus the join-heavy Q3/Q5/Q10/Q12/Q14 as they become expressible) match
DuckDB at SF0.1 and SF1; first honest numbers in `BENCHMARKS.md`.

### Phase 5 — Compression, zone maps, SIMD
Lightweight encodings chosen per segment (constant, RLE, dictionary, frame-of-reference +
bit-packing, delta), operating directly on compressed data where possible. Zone-map pruning of
row groups. AVX2 kernels (comparison → selection vector, sum/min/max, hashing, string prefix
compare) with runtime CPU dispatch and scalar fallback.

**Exit:** round-trip property tests for every encoding; before/after numbers for each kernel in
`BENCHMARKS.md`; memory footprint reduction measured on TPC-H lineitem.

### Phase 6 — Morsel-driven parallelism
Morsel dispatcher (shared atomic cursor) over a fixed thread pool; parallel scan, thread-local
pre-aggregation + partitioned merge, parallel hash-join build/probe, parallel sort + merge.

**Exit:** TSan clean; scaling curve (1–16 threads) for Q1/Q3/Q6 in `BENCHMARKS.md`; results are
bit-identical (or tolerance-identical) to single-threaded.

### Phase 7 — Persistence
On-disk columnar file format with checksums; WAL for appends; checkpointing; recovery. All I/O
behind a `FileSystem` interface so crashes can be injected deterministically.

**Exit:** crash-injection campaign (kill at every fsync/write boundary) never loses an
acknowledged commit nor surfaces a partial one; format fuzz target clean.

### Phase 8 — Optimizer, statistics, subqueries
Table/column statistics (row count, min/max, HyperLogLog NDV), filter and projection pushdown,
constant folding, join-order optimization (DP for small joins), build-side selection, subquery
unnesting (uncorrelated + the correlated shapes TPC-H needs), `EXPLAIN` and `EXPLAIN ANALYZE`
with per-operator rows and time.

**Exit:** all 22 TPC-H queries correct vs DuckDB; optimizer ablation (on/off) recorded.

### Phase 9 — Stretch
Choose by remaining time and target audience: spill-to-disk (external sort, grace hash join),
window functions, Postgres wire protocol so `psql` connects, Parquet reader.

---

## Engineering rules

1. **Test before push.** A milestone is pushed only after the full test suite passes locally with
   sanitizers. CI must be green on the pushed branch.
2. **Log every milestone** in [`PROGRESS.md`](PROGRESS.md): what changed, tests run and their
   results, numbers if performance-related, known gaps.
3. **Never weaken a test or a CI gate to make it pass.** If a test is wrong, fix it in its own
   commit with the reasoning in the message.
4. **Benchmarks are honest.** Record machine, compiler, flags, dataset, and the exact command.
   Report regressions, not just wins.
5. **Design before code** for anything non-obvious: an ADR in `docs/adr/`.
6. **Git:** one branch per phase (`phase-N-name`), small commits, branch pushed at each milestone,
   merged to `main` when the phase's exit criteria are verified.
