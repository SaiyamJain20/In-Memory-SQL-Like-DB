# ADR 0008 — Morsel-driven parallelism: participants, global/local state, partitioned merges

- **Status:** accepted
- **Date:** 2026-10-06

## Context
[ADR 0005](0005-push-pipelines-with-global-and-local-state.md) shaped operators for threads (global and
local state, `Combine`, `Finalize`) but ran each pipeline on one. Phase 6 runs them on many, on a
machine with 8 cores / 16 hardware threads, while keeping three properties: results are right, the
tests can see a race, and a query that cannot be parallelised (a `LIMIT`) still works.

## Decision

**Scheduler.** A fixed pool; a *job* is one function `body(participant)` that up to N threads each run
once, sharing work through whatever the body captures (an atomic cursor over morsels). The calling
thread is always participant 0, so a job makes progress when every worker is busy with another query
and nested jobs cannot deadlock; workers join while the job has room and it is still open; the first
exception is rethrown after everyone returned. There is no per-task queue and no work stealing: a
pipeline's unit of work is a morsel claimed from the source, which gives dynamic load balancing without one.

**Pipelines.** A pipeline runs on several threads only if its source, every streaming operator and its
sink say so (`ParallelSource` / `ParallelOperator` / `ParallelSink`; the default is "no", so an
operator that has not been audited is safe). `LIMIT` says no: it counts rows across chunks, and "the
first N rows" is only defined on one thread. Each participant creates its own local source state,
operator states and local sink state, pulls morsels, and merges into the global state in `Combine`. The
number of participants is bounded by the source (`MaxSourceThreads`, e.g. the number of morsels), so a
one-chunk query never pays for a pool. Pipelines still run one after another, each parallel inside.

**Morsels.** A table scan cuts the snapshot into morsels of 8 vectors (16,384 rows; never across a row
group, so zone-map pruning is unchanged) and threads claim them with `fetch_add`. A single thread
reading morsels in order sees exactly the chunks the sequential scan produced.

**Order.** Every chunk carries a *batch index* (its morsel number; an aggregate's range number; a
sorted chunk's position), assigned by the source and copied to the sink before each `Sink` call. All
chunks of a batch come from one thread, in order. The result collector and `INSERT` sort by it, so
`SELECT ... WHERE ...` over a scan returns rows in table order on any thread count, and `INSERT ...
SELECT` stores them in that order. What is **not** defined with more than one thread: the order of
groups out of a hash aggregate, the order of matches within one probe row of a join (it follows the build
side's row order, which is the order threads handed their stores over in), and the relative order of
rows with equal sort keys. SQL does not define these without `ORDER BY` either; with one thread
(the library default) they are unchanged and deterministic.

**Aggregates.** Each thread aggregates into a private `GroupTable` and hands it over. `Finalize` merges:
one table for small results, and from 32,768 groups by *hash partition* - the groups of all tables with
the same top hash bits are merged by one task (`GroupTable::CombineGroups`, built on
`AggregateState::CombineSubset`), so partitions merge in parallel and nothing is locked. `DISTINCT`
aggregates keep (group, value) pairs and merge whole tables only. The result is one table per partition,
served as a parallel source. **Integer `SUM` is exact:** a 128-bit accumulator, range-checked when the
result is produced, so the answer or the overflow error depends on the values only - not on the order
rows are added in, the SIMD kernels, or the thread count. (A running 64-bit total cannot promise that:
`{MAX, 1, -1}` would fail or succeed depending on which thread saw what. This changes the serial
behaviour too, to the one DuckDB has; ADR 0003 records it.)

**Joins.** Probing was already parallel (a probe state per thread over the finished table). The build:
`Combine` hands each thread's row stores over; `Finalize` moves whole chunks into one build side
(`ChunkStore::AdoptFullChunks`, only each thread's partial last chunk is copied), hashes key chunks in
parallel, then links rows into bucket chains **without atomics**: rows are scattered into per-bucket-
partition lists by two parallel passes and one task links one partition, so no two threads touch a
bucket, and chains keep ascending build order.

**Sort.** Threads buffer rows; `Finalize` adopts the chunks into one buffer and sorts it with a parallel
stable merge sort: ranges are sorted independently, then merged pairwise, each merge cut into
independent slices by binary search on the output position (co-rank), so the last, largest merges use
every thread. The result equals the serial stable sort of the same buffer exactly. Top-N prunes each
thread's buffer on that thread first.

**CSV.** `COPY` maps the file, finds record boundaries (quote-free files: a parallel newline index;
otherwise a serial state machine with `SplitRecord`'s rules, shared with the serial loader), and each
task parses, seals and compresses whole row groups; the groups are published in file order. Tables,
row groups and every error (line numbers included) equal the serial loader's, and the earliest error wins.

**Threads.** `Database` owns the pool; `SetThreads` swaps it (running queries keep theirs). The library
default is **one thread** - parallelism is opt-in (`CDB_THREADS`, `SetThreads`); the shell and the
benchmark default to all hardware threads.

**How it is tested.** Besides unit tests for each piece, the *whole* suite runs a second time in
`-parallel` mode (debug, release, asan, tsan): 4 threads, one-vector morsels, and every parallel
threshold at 1, so even the smallest query interleaves and takes the partitioned merge, the parallel
join build, the parallel sort and the parallel CSV path. ThreadSanitizer runs both modes.

## Consequences
- Scaling is bounded by Amdahl pieces that stay serial: the head of a pipeline (plan setup), `Combine`
  hand-overs, the open-address `KeyIndex` insert inside one thread's table, and a pipeline with `LIMIT`.
  Measured curves are in [BENCHMARKS](../BENCHMARKS.md).
- Memory grows with threads: one local aggregate table / sort buffer / build store per thread.
- Floating-point `SUM` / `AVG` *used to* re-associate across threads (and SIMD lanes): equal to the serial
  result only up to rounding, and not reproducible run to run. That broke a query that compares an
  aggregate with a recomputation of it (TPC-H Q15 found no row at 16 threads), so since Phase 8 they
  accumulate in compensated form (`common/compensated_sum.h`, ADR 0010 addendum below) and are rounded
  once: the same on any number of threads, with or without SIMD.
- Hash tables are not shared while building (no atomics, no latches), which keeps TSan quiet and the
  code readable, at the price of the partitioning passes.

## Alternatives considered
- **A task queue with work stealing (TBB / Cilk style):** more general, but a pipeline already has a
  natural unit of work (the morsel) and a shared cursor balances load; a stealing scheduler would add a
  queue per thread for nothing.
- **Exchange operators in a Volcano tree (partition / merge nodes):** parallelism becomes part of the
  plan, which complicates the optimizer; morsel-driven keeps plans serial and parallelises execution.
- **A shared concurrent hash table for aggregation or the join build (CAS on buckets):** no merge phase,
  but contention on hot groups and nondeterministic chain order; the partitioned merge is simpler to
  verify and scales with the number of partitions.
- **A 64-bit `SUM` with per-thread overflow checks:** cheaper, but the outcome would depend on thread
  scheduling; exactness in 128 bits costs one extra add-with-carry.
- **Deterministic floating-point sums** (fixed reduction tree over morsel numbers): possible later; not
  needed for TPC-H, and DuckDB does not promise it either.
