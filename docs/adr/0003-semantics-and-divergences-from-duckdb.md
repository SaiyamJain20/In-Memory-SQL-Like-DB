# ADR 0003 — SQL semantics: match DuckDB, with a short list of deliberate divergences

- **Status:** accepted
- **Date:** 2026-10-06

## Context
DuckDB is this project's correctness oracle (ADR 0001). SQL has many places where engines
disagree — integer division, overflow, casts, NULL ordering, string functions — so "behave like a
real database" needs a concrete reference. We pick DuckDB (which follows PostgreSQL where it
matters) and enforce it mechanically: `tools/gen_golden_expressions.py` asks DuckDB for the type
and value (or error) of ~4,300 expressions and `tests/planner/golden_expression_test.cpp` requires
this engine to agree on every one.

## Decision
Match DuckDB's observable behaviour, including its quirks, unless there is a recorded reason not
to. Behaviours adopted because the differential test showed them:

- `/` is always floating-point (`5/2 = 2.5`, `1/0 = inf`, `0.0/0 = nan`); `%` is C-style with
  `x % 0` = NULL for integers and NaN for doubles.
- Integer `+ - *` and negation are overflow-checked and raise; `abs(INT_MIN)` raises.
- `DOUBLE → integer` casts round half to even (`2.5 → 2`); `VARCHAR → integer` accepts decimals
  (`'2.5' → 3`), `0x` hex and `_` separators; out-of-range is an error.
- A string *literal* compared with a non-string is converted to that type
  (`date_col < '1998-01-01'`) but `'5' + 1` is an error.
- Integers are accepted as booleans in AND/OR/NOT; casting any NULL constant yields NULL.
- NULLs sort last for both ASC and DESC; `LIKE` has no escape character; `substring` follows
  DuckDB's treatment of start ≤ 0 and negative lengths.
- Doubles print like Python's `repr` (shortest round-trip digits, scientific outside
  `1e-4 ≤ |x| < 1e16`).

## Deliberate divergences (each listed where it is tested)
| Divergence | Why |
|---|---|
| `DECIMAL(p,s)`/`NUMERIC`/`FLOAT`/`REAL` are stored as `DOUBLE` | no fixed-point type yet; TPC-H results are compared with a tolerance. Revisit if exactness matters. |
| `SUM(INTEGER/BIGINT)` returns `BIGINT` (DuckDB: `HUGEINT`) and raises on overflow | no 128-bit type |
| `DATE` covers years 1–9999 only; no `TIMESTAMP`/`INTERVAL` types. `DATE ± INTERVAL` is folded for constant dates | scope; TPC-H only needs constant-date arithmetic |
| `UPPER`/`LOWER` map ASCII only | no Unicode case tables |
| Constant cast errors are raised while binding, even inside a branch that can never run (`FALSE AND CAST('x' AS INT) = 1`) | constants are folded eagerly for typing; DuckDB simplifies the boolean first |
| `NOT NULL` is enforced, other constraints (`PRIMARY KEY`, `UNIQUE`, …) are rejected as not implemented | honesty over silently ignoring them |

## Consequences
- Any new function or operator gets golden expressions generated from DuckDB before it is trusted.
- A divergence is never silent: it is either fixed or added to `KnownDivergences()` in the golden
  test (which fails if an entry starts matching, so the list cannot rot) and to this table.
