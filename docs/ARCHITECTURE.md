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

## Storage — [implemented: Phase 2; encodings planned: Phase 5]
Tables are lists of **row groups** (60 vectors = 122,880 rows). Each row group holds one
**column segment** per column.

**Segments are immutable and uncompressed [implemented]**, laid out exactly like a flat vector
(values + validity bitmask + sealed string heap, each padded to a whole number of vectors). A scan
therefore does no work per value: `ColumnSegment::Scan` points the output vector at precomputed
read-only `Buffer::View`s into the segment (`Vector::ReferenceFlat`), with no copy and no
allocation. Vectors obtained this way are read-only (asserted), and `Vector::Reset` detaches from
them instead of reusing them for writing. Encodings chosen at seal time (constant, RLE,
dictionary, frame-of-reference + bit-packing, delta) are **[planned: Phase 5]**; those segments
decode into the output vector (or, for dictionary encoding, expose it zero-copy as a dictionary
vector).

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
([ADR 0004](adr/0004-scalar-interpreter-as-reference-semantics.md)): it runs `INSERT … VALUES` and
table-free `SELECT`s today and is the oracle for Phase 4's vectorized kernels. It is checked
against DuckDB on ~4,300 generated expressions (`tests/planner/golden_expression_test.cpp`).

`Connection::Query` parses, binds and executes. DDL, `INSERT … VALUES` (atomic, via a staging
table merged in one step), `COPY … FROM` (CSV, atomic the same way), `EXPLAIN` and table-free
`SELECT` run now; queries over tables bind to a plan but need the Phase 4 executor. **12 of the 22
TPC-H queries bind completely**; the other 10 stop precisely at a subquery or `WITH`
(Phase 8).

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
