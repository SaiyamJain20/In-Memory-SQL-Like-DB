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
                                                            Phase 8
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
[implemented: Phase 1]; the expression kernels that consume it are [planned: Phase 4].*

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

## Storage — [planned: Phase 2, 5]
Tables are lists of **row groups** (~120k rows). Each row group holds one **column segment** per
column. Segments carry a **zone map** (min, max, null count) so scans can skip whole row groups
for predicates like `l_shipdate <= DATE '1998-09-02'`, and an **encoding** chosen at seal time
(constant, RLE, dictionary, frame-of-reference + bit-packing, delta; Phase 5). A scan takes a
projection (only requested columns are touched) and optional pushed-down filters.

The open tail of a table is a mutable buffer; it is sealed into immutable, compressed segments
when full. Immutable segments are what make cheap concurrent reads possible.

## Execution — [planned: Phase 4, 6]
**Push-based pipelines.** A query compiles to pipelines. Each is a `Source`, a chain of
streaming `Operator`s (filter, project, hash-probe) and a `Sink`. Pipeline breakers (hash
aggregate, join build, sort) are sinks that finalise before their dependent pipeline starts.

**Sink state is split** into a *global* state and a per-thread *local* state with a `Combine`
step. This is designed in from the first operator so Phase 6 (parallelism) adds a scheduler
rather than rewriting operators.

**Morsel-driven parallelism.** Sources hand out morsels (a row group, or a range of chunks) from
a shared atomic cursor; a fixed pool of workers each run the *whole* pipeline on their morsel
with thread-local state. Work stays cache-hot and there is no per-tuple synchronisation.

**Expressions.** Bound expression trees are evaluated by an `ExpressionExecutor` into vectors.
Predicates take a separate `Select` path that produces a selection vector directly, avoiding a
boolean vector and short-circuiting `AND` by narrowing the selection between conjuncts.

## Error handling
See [ADR 0002](adr/0002-error-handling.md): exceptions for query-level errors at module
boundaries (parse/bind/runtime), no exceptions in inner loops; the public API converts them to a
result with an error state.

## Testing strategy
| Layer | Tool |
|---|---|
| Unit | GoogleTest, every format × type × null combination for data structures |
| SQL | `sqllogictest`-style files in `tests/sql/` |
| Differential | random + TPC-H queries run on this engine and DuckDB; results diffed |
| Fuzz | libFuzzer on parser, later on file-format readers |
| Concurrency | ThreadSanitizer in CI |
| Crash safety | deterministic fault injection through the `FileSystem` interface (Phase 7) |
| Performance | Google Benchmark micro-benchmarks + TPC-H runner; results in `BENCHMARKS.md` |
