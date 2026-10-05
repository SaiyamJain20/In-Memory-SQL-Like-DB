# ADR 0001 — Rebuild as a columnar, vectorized analytics engine

- **Status:** accepted
- **Date:** 2026-10-06

## Context
The repository began as a ~600-line row-store prototype (`CREATE`/`INSERT`/`SELECT`, text log
replay, one mutex per table; see commit `a254830`). Verified defects included a recovery path that
corrupted data containing commas, `WHERE` clauses silently ignored depending on whitespace, and
no real type system. It also claimed multithreading without creating threads.

The goal is a flagship systems project for infrastructure / database-engine roles.

## Options considered
1. **Harden the row store** (fix bugs, add `UPDATE`/`DELETE`/`JOIN`). Cheap, but it stays a toy;
   feature count is not what database-engine reviewers look for.
2. **Transactional MVCC engine with WAL and Raft.** Deep and impressive, but the audience skews to
   OLTP / distributed-database teams.
3. **Columnar vectorized analytics engine.** Same depth in concurrency and storage, plus
   query-engine internals (vectorization, SIMD, compression, morsel parallelism, optimizer) that
   map directly to ClickHouse / Snowflake / Databricks / DuckDB / Velox-style teams.

## Decision
Option 3. The legacy code is removed (kept in git history) rather than evolved: its row-oriented
`vector<string>` representation is incompatible with every goal of the new design.

## Consequences
- Correctness is checkable against a strong oracle (DuckDB) and a standard workload (TPC-H).
- Performance claims are comparable and falsifiable.
- No transactions / MVCC in scope; storage is append-oriented with immutable sealed segments.
  Updates and deletes are out of scope until Phase 9 at earliest.
- Scope risk: TPC-H needs subqueries and a reasonable optimizer. Mitigated by phasing — the
  join-free and simple-join queries come first; all 22 are a Phase 8 exit criterion.
