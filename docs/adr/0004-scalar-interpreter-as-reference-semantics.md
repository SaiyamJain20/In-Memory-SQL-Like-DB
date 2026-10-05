# ADR 0004 — A row-at-a-time scalar interpreter is the reference semantics

- **Status:** accepted
- **Date:** 2026-10-06

## Context
Phase 4 adds vectorized expression kernels: typed, null-aware, selection-vector-aware, SIMD. They
are fast but intricate, and every subtle rule (three-valued logic, overflow, casts, LIKE) must be
implemented again for each type combination and vector format.

## Decision
Bound expressions (`BoundExpr`) have exactly one *reference* implementation, `EvaluateScalar`: a
simple, obviously-correct, row-at-a-time interpreter over `Value`s. It is validated against DuckDB
(ADR 0003). It is used today to run `INSERT … VALUES` and table-free `SELECT`s, and to constant-fold
during binding. Phase 4's vectorized kernels are tested by running random expression trees over
random vectors (flat / constant / dictionary, with NULLs) and requiring bit-for-bit agreement
with `EvaluateScalar` row by row.

## Consequences
- Correctness work happens once, in readable code; kernels are checked mechanically.
- Folding at bind time and evaluation at run time share semantics by construction.
- The interpreter is deliberately slow and must never appear on a query's hot path.
