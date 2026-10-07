# ADR 0006 — A small rule-based logical optimizer before statistics exist

- **Status:** accepted; the join-ordering rule (item 2) is superseded by [ADR 0010](0010-statistics-subqueries-and-cost-based-joins.md) (statistics and cost-based ordering, Phase 8)
- **Date:** 2026-10-06

## Context
Running the binder's plan as-is is correct but slow: `FROM a, b, c WHERE …` becomes a cross product
with a filter on top (TPC-H Q19 took 46 s at SF0.01 for exactly this reason), every scan reads every
column, and `LIMIT` sits above `ORDER BY` behind a projection so it cannot become a top-N. A full
cost-based optimizer needs statistics (Phase 8), but a handful of rules removes the catastrophic
plans now.

## Decision
`Optimize()` applies, in order:

1. **Filter pushdown.** Conjuncts move toward the scans: through projections (by substituting the
   projected expressions), onto group-by columns, through `ORDER BY`/`DISTINCT`, and into the side of
   an inner join they reference. For outer joins a `WHERE` conjunct only goes to the *preserved* side
   (anything else changes which rows are padded), and an `ON` conjunct that mentions only the
   *padded* side may go below that side. `column op constant` conjuncts above a scan also become
   zone-map pruning hints.
2. **Join ordering.** A tree of inner/cross joins is flattened into relations and conjuncts and
   rebuilt left-deep: start from the largest estimated relation (it is the probe side and streams),
   then repeatedly join the relation with the smallest estimate that is connected by an equality
   predicate (so builds stay small and cross products appear only if the query has no connecting
   predicate). Estimates are table row counts times fixed selectivities. Outer joins are barriers.
3. **OR factoring.** `(a AND x) OR (a AND y)` becomes `a AND (x OR y)`, which exposes equi-join keys
   hidden inside a disjunction. This is valid in three-valued logic (distribution and absorption hold).
4. **LIMIT below projection**, so `LIMIT n` directly over `ORDER BY` plans as a top-N.
5. **Column pruning**, top-down, so scans read and operators carry only the columns used above.

The output columns of the plan (names, types, order) never change; a join reorder adds a projection
that restores the original column order.

Correctness is checked differentially: a randomized test runs thousands of queries (all join kinds,
derived tables, aggregates, NULL-heavy data) with the optimizer on and off and requires identical
results; `Connection::SetOptimizerEnabled(false)` runs the plan exactly as bound.

## Consequences
- Plans are predictable and explainable (`EXPLAIN` shows the optimized plan); each rule is local.
- Estimates are crude (no NDV, no histograms): the greedy order can be worse than a cost-based one.
  Phase 8 replaces the estimates and adds DP enumeration for small join graphs; the structure
  (flatten → order → rebuild) stays.
- Subqueries are not yet unnested (Phase 8), so 10 of the 22 TPC-H queries still do not run.
