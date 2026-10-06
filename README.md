# cdb — a columnar, vectorized SQL analytics engine in C++20

A from-scratch analytical query engine in the style of DuckDB / ClickHouse / Velox: columnar
storage, vector-at-a-time execution, morsel-driven parallelism, SIMD kernels, and a cost-based
optimizer — built to be small enough to read end to end, and verified against DuckDB on TPC-H.

> **Status: early development (Phases 0–6 of 9 complete).** The engine runs SQL end to end: a
> hand-written parser and binder, a rule-based optimizer, a push-based vectorized executor
> (hash aggregation, hash joins, sort/top-N) over compressed columnar storage (bit-packing, RLE,
> dictionaries, lossless scaled doubles) with zone-map pruning and AVX2 kernels behind runtime CPU
> dispatch, and morsel-driven parallelism across a thread pool (parallel scans, partitioned
> aggregation, parallel join build, parallel merge sort, parallel CSV loading; TSan-clean).
> It runs the 12 TPC-H queries that need no subqueries and **matches DuckDB's answers on all of
> them at SF0.01, SF0.1 and SF1**; the whole test suite passes unchanged with SIMD or compression
> switched off, and again in a stress mode that runs every query on 4 threads with one-vector
> morsels. On one thread the SF1 geometric mean is 2.6x DuckDB's time; at 16 threads
> (8 cores) the engine is **5.5x faster than on one thread** (Q1 6.4x, Q6 5.8x) and 1.7x DuckDB's
> time at the same thread count; the 8x target was not met. `DISTINCT` aggregates do not scale yet.
> Persistence (Phase 7) and subqueries/statistics (Phase 8) are still ahead. Every number is in
> [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) with machine, build and command; see the
> [roadmap](docs/ROADMAP.md) and the dated log [`docs/PROGRESS.md`](docs/PROGRESS.md).

## Try it
```bash
cmake --preset release && cmake --build --preset release
build/release/tools/cdb_shell
cdb> CREATE TABLE t (a INTEGER, b VARCHAR);
cdb> INSERT INTO t VALUES (1, 'x'), (2, 'y'), (3, 'x');
cdb> SELECT b, count(*), sum(a) FROM t GROUP BY b ORDER BY b;
cdb> EXPLAIN SELECT b FROM t WHERE a > 1 ORDER BY b LIMIT 2;
```
TPC-H: `python3 -m venv .venv && .venv/bin/pip install -r tools/requirements-dev.txt`, then
`.venv/bin/python tools/tpch_data.py --sf 1` and `build/release/bench/cdb_tpch --sf 1`
(`tools/tpch_duckdb_time.py` times DuckDB the same way).

## Goals
- **Correct** — differential-tested against DuckDB; all 22 TPC-H queries.
- **Fast** — vectorized kernels, compressed columnar storage, parallel execution; honest
  benchmark numbers with machine, compiler and command recorded.
- **Understandable** — every major decision has an [ADR](docs/adr/); the
  [architecture](docs/ARCHITECTURE.md) document tracks what is implemented vs. planned.

## Build
Requires CMake ≥ 3.21, Ninja, and GCC 13+ or Clang 18+ (C++20).

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Other presets: `release`, `asan` (AddressSanitizer + UBSan), `tsan` (ThreadSanitizer).
See [`CLAUDE.md`](CLAUDE.md) for the full developer workflow.

## Repository history
This repository began as a ~600-line row-oriented prototype (`CREATE`/`INSERT`/`SELECT` with a
text-log replay). It was retired in favour of the columnar rewrite; the original is preserved in
git history at commit `a254830`. The reasoning is recorded in
[ADR 0001](docs/adr/0001-columnar-vectorized-direction.md).
