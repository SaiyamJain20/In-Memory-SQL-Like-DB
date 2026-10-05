# cdb — a columnar, vectorized SQL analytics engine in C++20

A from-scratch analytical query engine in the style of DuckDB / ClickHouse / Velox: columnar
storage, vector-at-a-time execution, morsel-driven parallelism, SIMD kernels, and a cost-based
optimizer — built to be small enough to read end to end, and verified against DuckDB on TPC-H.

> **Status: early development (Phases 0–3 of 9 complete).** Build, CI, the vector/type system, the
> columnar storage layer (zone maps, snapshot scans, catalog) and the SQL front end (parser, binder,
> logical plans, DuckDB-verified expression semantics, CSV loading, a shell) are in place;
> queries over tables are bound but not yet executable - the vectorized executor is Phase 4. The engine is being built phase by phase. Nothing below is
> claimed until it is implemented *and* measured — see the [roadmap](docs/ROADMAP.md) for what is
> done and [`docs/PROGRESS.md`](docs/PROGRESS.md) for the dated log.

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
