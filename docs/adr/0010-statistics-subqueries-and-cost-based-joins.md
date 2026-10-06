# ADR 0010 — Statistics, subquery unnesting, cost-based join ordering, EXPLAIN ANALYZE

- **Status:** accepted (supersedes the join-ordering rule of [ADR 0006](0006-rule-based-optimizer.md))
- **Date:** 2026-10-07

## Context
After Phase 7 the engine ran 12 of the 22 TPC-H queries: the other 10 need subqueries or `WITH`. Its
join order came from fixed selectivities and a zone-map guess at distinct counts, and nothing showed
how far an estimate was from reality. Phase 8 closes all three.

## Decision

### 1. Subqueries are unnested while binding (no subquery operator at run time)
`WITH` (non-recursive) is inlined through a scope chain. A subquery becomes joins:

| Shape | Plan |
|---|---|
| `[NOT] EXISTS (...)` as an AND-ed `WHERE` conjunct | semi / anti join; the correlation `inner = outer` conjuncts are the join keys, everything else the residual |
| `x [NOT] IN (SELECT ...)` as an AND-ed conjunct | semi join; `NOT IN` is a **null-aware** anti join (three-valued logic: a NULL anywhere on the right, or a NULL `x` against a non-empty right, makes the row unknown, hence dropped; an empty right keeps every row) |
| uncorrelated scalar `(SELECT ...)` | cross join with the one-row result; a `ScalarGuard` operator unless the plan is one row by construction (an ungrouped aggregate): one row, NULL if none, an error if several |
| correlated scalar aggregate `(SELECT agg(..) FROM .. WHERE inner = outer ..)` | `LEFT` join of the outer rows with the aggregate **grouped by the correlation keys**; an outer row with no group gets the aggregate's value over no rows (`count`: 0, others NULL) - the "count bug" |

Shapes outside this table fail with `NotImplemented` at the offending expression (correlated `NOT IN`,
correlated non-aggregate scalars, `IN` / `EXISTS` under `OR` or `NOT`, correlation two levels up,
non-equality correlation of a scalar). The optimizer sinks a semi / anti join below an inner join to
the side of the join it mentions, and keeps it above the null-supplied side of an outer join.

### 2. Statistics
Every sealed column segment carries a **HyperLogLog** sketch of its non-NULL values (4096 one-byte
registers, 1.6% standard error), computed from the raw values at seal time (so it parallelises with
the row group build) and stored in the checkpoint (format version 2) in whichever of a sparse or a
dense form is smaller. `Table::Statistics()` merges the zone maps (bounds, NULL counts) and the
sketches (a merge is the per-register maximum, exact for the union) into per-column statistics,
caches them by table version, and reads the frozen copy of an open tail on demand.
`Table::Merge` now continues the open row group for a small load instead of sealing a short one per
statement (a table filled by single-row `INSERT`s would otherwise carry a sketch per row).

### 3. Cardinality estimation (`planner/cardinality`)
Rules, all in one place and all tested by arithmetic and against real row counts:
equality `1/distinct` of the non-NULL rows (0 outside the bounds); ranges over a **grid** of
`distinct` equally spaced values between min and max (dense integer domains snapped to their exact
size; a continuous model estimated `qty > 49` over 1..50 at nothing); a lower and an upper bound on one
column form an **interval** (P(< b) - P(< a)), not two independent shares; `AND` independent, `OR` by
inclusion-exclusion; `IN`, `LIKE`, `IS NULL`; filters narrow the bounds and distinct counts they
constrain; joins by containment (`1 / max(distinct)` per key, a composite key capped by the larger
input's rows); semi / anti joins from the fraction of left keys that have a partner; aggregates from
the product of the group columns' distinct counts. Estimates are per operator, memoised per plan.

### 4. Join ordering (`planner/join_order`)
The optimizer flattens a tree of inner / cross joins into relations (with their own filters pushed
down and estimated) and predicates, hands the *graph* (sizes and predicates) to the search, and
rebuilds the plan from the tree it returns. The search is dynamic programming over subsets, **bushy
trees included**, up to 12 relations (beyond that, a greedy left-deep order). Cost = rows out +
2 x rows built + rows probed; the result size of a set of relations is independent of the order
(product of sizes times the selectivities of the predicates inside the set), so subplans for one set
are comparable. The smaller input of every join is the build side (the larger probes). A set that no
predicate can join is never combined with another by a join some predicate could have made, so a
connected query never gets a cross product; sets that need one are the only ones allowed it. Ties go
to the relation written first (within rounding), so estimation noise decides nothing.

### 5. EXPLAIN / EXPLAIN ANALYZE
`EXPLAIN` prints the optimized plan with `(~N rows)` per operator. `EXPLAIN ANALYZE <select>` runs it
and prints `(est ~E, actual A rows, T ms)`, a join's build side, the rows a sink consumed, and
planning / execution time and thread count. The executor takes an optional profile (relaxed
atomic counters per operator, one timer per call; a null-pointer check when off); the physical
planner remembers which logical operator each physical one came from. Times are CPU time summed over
threads and cover an operator's own calls only.

### 6. Floating-point SUM / AVG are order-independent (found by the Phase 8 benchmark)
TPC-H Q15 returned no row at 16 threads on SF1: `WITH revenue AS (... sum(...) ...) ... WHERE
total_revenue = (SELECT max(total_revenue) FROM revenue)` evaluates the aggregate twice (a CTE is
inlined per reference), and a parallel floating-point sum differs in the last bits between two
evaluations (4,261 of 10,000 supplier sums, at 4 threads). `SUM` and `AVG` of a DOUBLE now accumulate in
compensated form (`CompensatedSum`: an unevaluated `hi + lo`, every addition a TwoSum whose rounding
error goes to `lo`, partial sums merged the same way, the AVX2 kernel with `(hi, lo)` lanes) and round
once at the end, so the result is the exact sum rounded, except for sums within 2^-100 of a rounding
boundary: the same on any number of threads and morsel sizes, with or without SIMD. Cost on SF1,
one thread: Q1 (six such aggregates over 6 M rows, 4 groups) +23%, Q18 +11%, Q6 (ungrouped, vectorized)
none; DuckDB also uses compensated (Kahan) summation for `SUM(DOUBLE)`.

## Verification
- The 22 TPC-H queries match DuckDB (SF0.01 on memory / checkpoint / log in the gate; SF0.1 and SF1 by hand).
- Subquery SQL files (102 + CTE queries, DuckDB-generated expected results) run in memory and across a
  power cut after every statement; operators are checked against a three-valued `NOT IN` reference.
- **Random differential fuzzing** (`tools/fuzz_sql.py`): random schemas and queries (joins of every
  kind, aggregates, derived tables, CTEs, every subquery shape, interleaved inserts) with DuckDB's
  answers; each runs with the optimizer on and off. The gate runs 1,200 queries; campaigns of tens of
  thousands are manual. It found a bug in the new join-tree rebuild (a predicate with no columns).
- The join search is checked against an explicit enumeration of all trees on random graphs; the
  estimator against true counts on random tables; the optimizer on vs off on thousands of random
  queries with subquery conjuncts; 48 mutants for the new code.

## Consequences
- Estimates only have to rank plans, and are wrong where independence is: correlated predicates, a
  scalar subquery's value (unknown at plan time: a range against it is estimated at a third), skew.
  `EXPLAIN ANALYZE` shows where. Histograms are future work.
- A semi / anti join always builds the subquery side. When the outer side is much smaller (TPC-H Q4)
  the opposite build (a "right semi" join with match flags on the build side) would win; not done.
- `INTERVAL` arithmetic is still constant-date only; `UPDATE` / `DELETE` and persistent statistics
  beyond the sketches are not there.
- Bushy plans can build on a join result; a plan's hash tables are still all in memory (no spilling).

## Alternatives considered
- **A subquery operator evaluated per row** (nested loops): simple, quadratic; unnesting is what the
  TPC-H queries need and what makes the plans joins the optimizer can reorder.
- **Histograms and most-common-values lists:** more accurate on skew; the sketches and bounds already
  separate the plans that matter here, and a histogram per segment would have to be merged too.
- **Left-deep only:** the bushy trees the search finds (a small dimension joined to a medium one
  first, the result built against the big table) are the ones that win on TPC-H Q7 / Q8 / Q2.
- **DPccp / graph-based enumeration:** needed beyond ~12 relations; a subset DP is simpler and enough.
