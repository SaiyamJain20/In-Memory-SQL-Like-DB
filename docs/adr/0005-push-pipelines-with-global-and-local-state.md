# ADR 0005 — Push-based pipelines with global and local operator state

- **Status:** accepted
- **Date:** 2026-10-06

## Context
The executor has to run joins, aggregations and sorts over columnar vectors, and Phase 6 will run
them on many threads. The two classic designs are pull-based (Volcano: each operator asks its child
for the next chunk) and push-based (a pipeline pushes chunks from a source through operators into a
sink). Pull makes parallelism awkward: operators own the control flow, so a shared operator tree has
to be cloned or locked. It also makes pipeline breakers (hash build, sort) implicit.

## Decision
A query compiles to **pipelines**, `source → streaming operator* → sink`, executed in dependency
order. Operators are written to a three-role protocol:

- **source**: `GetData(global, local, out)`; a pipeline breaker is the *sink* of one pipeline and the
  *source* of the next, reading its own finished global sink state;
- **streaming operator**: `Execute(state, input, output)` returning `NeedMoreInput`,
  `HaveMoreOutput` (the same input produces more output, e.g. a probe row with many matches) or
  `Finished` (a satisfied LIMIT stops the whole pipeline);
- **sink**: `Sink(global, local, chunk)`, `Combine(global, local)` once per thread, `Finalize(global)`
  once after all combines.

State is split: **global** state is shared by every thread working on the pipeline; **local** state is
private to one. A hash aggregate aggregates into a thread-local table and merges it in `Combine`;
a join build collects rows locally and publishes them in `Combine`/`Finalize`. Phase 4 runs one local
state per pipeline; the tests already merge partial tables (`GroupTable::Combine`), so Phase 6 adds a
scheduler and morsel dispatch, not a rewrite.

Operators exchange data only as `DataChunk`s of at most 2048 rows. Filters and join probes emit
**zero-copy dictionary views** (shared column buffers plus a selection vector) instead of copying;
sinks that keep rows copy them into a `ChunkStore`.

## Consequences
- Pipeline breakers are explicit, which makes memory and parallelism easy to reason about.
- Early termination (LIMIT) is a first-class result, not an exception or a flag in each operator.
- Streaming operators must not retain their input chunk across calls: the source reuses its buffers.
  Operators that need to resume (join probe) keep *positions* in their state, never pointers into
  the input.
- A chunk that was sliced stays valid only until its producer runs again; the executor processes each
  chunk to the sink before pulling the next.

## Alternatives considered
- **Pull-based Volcano with vectors**: simpler to write, harder to parallelise, and breakers hide.
- **Compiled (JIT) pipelines**: fastest, but a large engineering and portability cost for this
  project's goals; vectorized interpretation gets most of the benefit.
