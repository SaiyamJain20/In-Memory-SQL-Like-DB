#!/usr/bin/env python3
"""Generates tests/data/golden_expressions.tsv: DuckDB's answers for ~1000 constant SQL expressions.

The C++ test (tests/planner/golden_expression_test.cpp) runs every expression through this
engine's parser, binder and scalar evaluator and compares the type and value with DuckDB's. That
makes DuckDB the oracle for expression semantics: NULL logic, overflow, casts, string functions,
date arithmetic.

Each output line:  <expression> TAB <type> TAB <value>
  type   BOOLEAN | INTEGER | BIGINT | DOUBLE | VARCHAR | DATE, or OTHER when DuckDB's type has no
         counterpart here (DECIMAL, HUGEINT, ...) - then only the value is compared
  value  NULL | true | false | integer | float repr (nan, inf, -inf) | ISO date | JSON string
         | ERROR (DuckDB raised an error: this engine must raise one too)

Only expressions whose DuckDB semantics this engine intends to match belong here; deliberate
divergences are listed in docs/ARCHITECTURE.md (DECIMAL is DOUBLE, SUM(INTEGER) is BIGINT, ASCII-only
UPPER/LOWER, ...). Regenerate with:  .venv/bin/python tools/gen_golden_expressions.py
"""
import datetime
import itertools
import json
import math
import pathlib

import duckdb

OUT = pathlib.Path(__file__).resolve().parent.parent / "tests" / "data" / "golden_expressions.tsv"
KNOWN = {"BOOLEAN", "INTEGER", "BIGINT", "DOUBLE", "VARCHAR", "DATE"}

exprs: list[str] = []


def add(*items):
    exprs.extend(items)


# ---------------------------------------------------------------------------- operand grids
INT = ["0", "1", "-1", "7", "-7", "2147483647", "(-2147483647)", "NULL"]
BIG = ["CAST(0 AS BIGINT)", "CAST(5 AS BIGINT)", "CAST(-3 AS BIGINT)", "CAST(9223372036854775807 AS BIGINT)",
       "CAST(-9223372036854775807 AS BIGINT)", "4294967296"]
DBL = ["0e0", "1.5e0", "-2.5e0", "1e308", "CAST('nan' AS DOUBLE)", "CAST('inf' AS DOUBLE)",
       "CAST('-inf' AS DOUBLE)", "5e-324"]
ARITH = ["+", "-", "*", "/", "%"]
CMP = ["=", "<>", "<", "<=", ">", ">="]

for op in ARITH + CMP:
    for a, b in itertools.product(INT, INT):
        add(f"{a} {op} {b}")
    for a, b in itertools.product(BIG, INT[:5] + BIG):
        add(f"{a} {op} {b}")
    for a, b in itertools.product(DBL, DBL + INT[:4] + BIG[:3]):
        add(f"{a} {op} {b}")
for a, b in itertools.product(INT[:5] + BIG[:3] + DBL[:4], repeat=2):
    add(f"{a} = {b}", f"{a} < {b}")

# ---------------------------------------------------------------------------- 3-valued logic
TV = ["TRUE", "FALSE", "NULL"]
for a, b in itertools.product(TV, TV):
    add(f"{a} AND {b}", f"{a} OR {b}", f"({a} = {b})")
for a in TV:
    add(f"NOT {a}", f"{a} IS NULL", f"{a} IS NOT NULL")
add("NOT NULL IS NULL", "1 = 1 AND 2 = 2 OR 3 = 4", "NOT (1 = 2) AND NOT (2 = 3)",
    "TRUE AND (1 / 0e0 > 1)", "FALSE AND CAST('x' AS INTEGER) = 1", "NULL AND FALSE", "NULL OR TRUE")

# ---------------------------------------------------------------------------- casts
CAST_VALUES = {
    "BOOLEAN": ["TRUE", "FALSE", "NULL"],
    "INTEGER": ["0", "1", "-1", "2147483647", "(-2147483647)", "NULL"],
    "BIGINT": ["CAST(0 AS BIGINT)", "CAST(5000000000 AS BIGINT)", "CAST(-5000000000 AS BIGINT)",
               "CAST(2147483647 AS BIGINT)", "CAST(2147483648 AS BIGINT)", "CAST(NULL AS BIGINT)"],
    "DOUBLE": ["0e0", "0.5e0", "1.5e0", "2.5e0", "3.5e0", "-0.5e0", "-1.5e0", "-2.5e0", "2.4999e0",
               "2147483647e0", "2147483648e0", "1e10", "1e19", "CAST('nan' AS DOUBLE)",
               "CAST('inf' AS DOUBLE)", "123456789.123e0", "1e-7", "1e20"],
    "DATE": ["DATE '1998-12-01'", "DATE '0001-01-01'", "DATE '9999-12-31'", "CAST(NULL AS DATE)"],
    "VARCHAR": ["'0'", "'42'", "'-17'", "'+7'", "'  12  '", "'1.5'", "'2.5'", "'-2.5'", "'1e2'", "'1E-2'",
                "'abc'", "''", "' '", "'12abc'", "'0x10'", "'inf'", "'-Infinity'", "'NaN'", "'true'", "'T'",
                "'yes'", "'no'", "'false'", "'1'", "'0'", "'2'", "'1998-12-01'", "'1998-13-01'",
                "'1998-12-1'", "' 1998-12-01 '", "'2020-02-30'", "'99999999999'", "'2147483648'",
                "'-2147483648'", "'9223372036854775807'", "'9223372036854775808'", "'1e999'", "'.5'", "'5.'",
                "'1_000'", "CAST(NULL AS VARCHAR)"],
}
for tgt in ["BOOLEAN", "INTEGER", "BIGINT", "DOUBLE", "DATE", "VARCHAR"]:
    for src, values in CAST_VALUES.items():
        for v in values:
            add(f"CAST({v} AS {tgt})")
add("CAST(2.5e0 AS INTEGER) + 1", "'5'::INTEGER + 1", "CAST(CAST(1e20 AS VARCHAR) AS DOUBLE)",
    "CAST(CAST(0.1e0 AS VARCHAR) AS DOUBLE) = 0.1e0", "CAST(1 AS VARCHAR) || CAST(2 AS VARCHAR)")

# ---------------------------------------------------------------------------- mixed-type comparisons
add("1 = '1'", "1 < '2'", "'3' > 2", "2.5e0 = '2.5'", "DATE '2020-01-01' < '2020-01-02'",
    "DATE '2020-01-01' = '2020-01-01'", "'2020-01-02' > DATE '2020-01-01'", "TRUE = 'true'",
    "'a' < 'B'", "'a' < 'b'", "'' < 'a'", "'abc' < 'abd'", "'abc' < 'abcd'", "'Z' < 'a'",
    "NULL = NULL", "NULL <> NULL", "NULL < 1", "1 = NULL", "'a' = NULL", "NULL = DATE '2020-01-01'",
    "1 = 1e0", "1 < 2.5e0", "CAST(5 AS BIGINT) = 5", "CAST(5 AS BIGINT) < 5.5e0",
    "'10' < '9'", "'x' = 'x'", "DATE '2020-02-29' > DATE '2020-02-28'")

# ---------------------------------------------------------------------------- BETWEEN / IN / CASE / COALESCE
for x in ["5", "1", "10", "0", "11", "NULL"]:
    add(f"{x} BETWEEN 1 AND 10", f"{x} NOT BETWEEN 1 AND 10")
add("5 BETWEEN 10 AND 1", "NULL BETWEEN 1 AND 2", "3 BETWEEN NULL AND 5", "3 BETWEEN 1 AND NULL",
    "9 BETWEEN NULL AND 5", "'b' BETWEEN 'a' AND 'c'", "DATE '2020-06-01' BETWEEN DATE '2020-01-01' AND DATE '2020-12-31'",
    "2.5e0 BETWEEN 2 AND 3", "CAST(5 AS BIGINT) BETWEEN 1 AND 10")
for x in ["1", "3", "NULL"]:
    for lst in ["(1, 2)", "(3, 4)", "(1, NULL)", "(NULL)", "(2, NULL, 3)", "(1, 1, 1)"]:
        add(f"{x} IN {lst}", f"{x} NOT IN {lst}")
add("'a' IN ('a', 'b')", "'c' IN ('a', 'b')", "'a' IN ('a', NULL)", "'c' NOT IN ('a', NULL)",
    "2 IN (1, 2.5e0, 2e0)", "2.5e0 IN (1, 2, 3)", "DATE '2020-01-01' IN ('2020-01-01', '2021-01-01')",
    "CAST(5 AS BIGINT) IN (1, 5)", "1 IN (CAST(1 AS BIGINT))")
add("CASE WHEN 1 = 1 THEN 'a' ELSE 'b' END", "CASE WHEN 1 = 2 THEN 'a' ELSE 'b' END",
    "CASE WHEN 1 = 2 THEN 'a' END", "CASE WHEN NULL THEN 1 ELSE 2 END", "CASE WHEN 1 = 2 THEN 1 WHEN 2 = 2 THEN 2 ELSE 3 END",
    "CASE 2 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'many' END", "CASE 5 WHEN 1 THEN 'one' END",
    "CASE NULL WHEN NULL THEN 1 ELSE 2 END", "CASE 1 WHEN 1 THEN 1.5e0 ELSE 2 END",
    "CASE WHEN TRUE THEN 1 ELSE CAST(2 AS BIGINT) END", "CASE WHEN FALSE THEN 1 ELSE 2.5e0 END",
    "CASE WHEN TRUE THEN NULL ELSE 1 END", "CASE WHEN FALSE THEN 1 WHEN NULL THEN 2 ELSE 3 END",
    "CASE WHEN TRUE THEN 1 ELSE 2147483647 + 1 END")
add("COALESCE(NULL, 1, 2)", "COALESCE(NULL, NULL, 3)", "COALESCE(1, 1 / 0e0)", "COALESCE(NULL, 'x')",
    "COALESCE(NULL, 2.5e0, 1)", "COALESCE(CAST(NULL AS BIGINT), 5)", "COALESCE(NULL, DATE '2020-01-01')",
    "NULLIF(1, 1)", "NULLIF(1, 2)", "NULLIF(NULL, 1)", "NULLIF(1, NULL)", "NULLIF('a', 'a')", "NULLIF('a', 'b')",
    "NULLIF(1.5e0, 1.5e0)", "NULLIF(5, CAST(5 AS BIGINT))")

# ---------------------------------------------------------------------------- strings
add("'a' || 'b'", "'a' || NULL", "NULL || 'a'", "'a' || 1", "1 || 2", "'x' || 1.5e0", "'x' || TRUE",
    "'x' || DATE '2020-01-05'", "'' || ''", "'é' || 'ü'", "'a' || CAST(5 AS BIGINT)")
LIKE_CASES = [("abc", "abc"), ("abc", "a%"), ("abc", "%c"), ("abc", "%b%"), ("abc", "a_c"), ("abc", "___"),
              ("abc", "____"), ("abc", "__"), ("abc", "%"), ("", "%"), ("", ""), ("", "_"), ("abc", ""),
              ("abc", "ABC"), ("ABC", "a%"), ("a%c", "a%c"), ("a_c", "a_c"), ("ab", "a%b"), ("axxb", "a%b"),
              ("aXb", "a_b"), ("abab", "%ab"), ("abab", "%ba%"), ("mississippi", "m%iss%pi"),
              ("mississippi", "%ss%ss%"), ("héllo", "h_llo"), ("héllo", "h__llo"), ("日本語", "日_語"),
              ("日本語", "___"), ("abc", "a%%c"), ("abc", "%%%"), ("a.c", "a.c"), ("a\\c", "a\\c"),
              ("special requests", "%special%requests%"), ("requests special", "%special%requests%")]
for text, pat in LIKE_CASES:
    t, p = "'" + text.replace("'", "''") + "'", "'" + pat.replace("'", "''") + "'"
    add(f"{t} LIKE {p}", f"{t} NOT LIKE {p}")
add("NULL LIKE 'a'", "'a' LIKE NULL", "NULL LIKE NULL", "NULL NOT LIKE 'a'")
for s, a, b in [("hello", 1, 2), ("hello", 2, 3), ("hello", 0, 2), ("hello", -1, 3), ("hello", -3, 2),
                ("hello", 10, 2), ("hello", 5, 5), ("hello", 6, 1), ("hello", 2, 0), ("hello", 2, -1),
                ("hello", 3, -5), ("hello", -7, 3), ("hello", -5, 5), ("hello", -6, 2), ("", 1, 1),
                ("héllo", 2, 3), ("日本語テキスト", 2, 3), ("hello", 1, 100), ("hello", 0, 1), ("hello", -1, 1)]:
    add(f"SUBSTRING('{s}', {a}, {b})", f"SUBSTRING('{s}' FROM {a} FOR {b})")
for s, a in [("hello", 1), ("hello", 2), ("hello", 5), ("hello", 6), ("hello", 0), ("hello", -2), ("hello", -9),
             ("héllo", 3), ("", 1)]:
    add(f"SUBSTRING('{s}', {a})", f"SUBSTRING('{s}' FROM {a})")
add("SUBSTRING(NULL, 1, 2)", "SUBSTRING('abc', NULL, 2)", "SUBSTRING('abc', 1, NULL)", "SUBSTRING('abc', CAST(2 AS BIGINT))",
    "SUBSTRING('abc', 1, CAST(2 AS BIGINT))")
for s in ["", "a", "hello", "héllo", "日本語", "a b  c", "x" * 40, "emoji😀"]:
    add(f"LENGTH('{s}')")
for s in ["", "abc", "ABC", "AbC dEf", "a1b2", "x-y_z"]:
    add(f"UPPER('{s}')", f"LOWER('{s}')")
add("LENGTH(NULL)", "UPPER(NULL)", "LOWER(NULL)")

# ---------------------------------------------------------------------------- numeric functions
for x in ["5", "-5", "0", "(-2147483647)", "CAST(-9223372036854775807 AS BIGINT)", "CAST(-9223372036854775808 AS BIGINT)",
          "2.5e0", "-2.5e0", "0e0", "-0e0", "CAST('nan' AS DOUBLE)", "CAST('-inf' AS DOUBLE)", "NULL"]:
    add(f"ABS({x})")
for x in ["0.5e0", "1.5e0", "2.5e0", "-0.5e0", "-1.5e0", "-2.5e0", "2.4e0", "2.6e0", "1e300", "CAST('nan' AS DOUBLE)",
          "CAST('inf' AS DOUBLE)", "0e0", "7", "NULL"]:
    add(f"ROUND({x})", f"FLOOR({x})", f"CEIL({x})")
for x, d in [("2.567e0", 2), ("2.567e0", 0), ("2.567e0", 1), ("2.5e0", 0), ("-2.567e0", 2), ("1234.5678e0", -2),
             ("1234.5678e0", -1), ("1234.5678e0", -5), ("0.5e0", 0), ("1.005e0", 2), ("123456.789e0", 3),
             ("5e0", 2), ("NULL", 2), ("2.567e0", "NULL")]:
    add(f"ROUND({x}, {d})")

# ---------------------------------------------------------------------------- dates
DATES = ["1970-01-01", "1969-12-31", "1998-12-01", "2000-02-29", "1900-02-28", "2024-02-29", "2023-12-31",
         "0001-01-01", "9999-12-31", "2020-03-31", "2020-01-31", "1996-07-15"]
for d in DATES:
    for fn in ["YEAR", "MONTH", "DAY", "DAYOFWEEK", "DAYOFYEAR", "QUARTER"]:
        add(f"{fn}(DATE '{d}')")
    for field in ["year", "month", "day", "dow", "doy", "quarter"]:
        add(f"EXTRACT({field} FROM DATE '{d}')")
    add(f"CAST(DATE '{d}' AS VARCHAR)", f"DATE '{d}' + 1", f"DATE '{d}' - 1", f"1 + DATE '{d}'",
        f"DATE '{d}' + 365", f"DATE '{d}' - 365", f"DATE '{d}' + CAST(30 AS BIGINT)")
for a, b in itertools.product(["2020-01-01", "2019-12-31", "2020-03-01", "1970-01-01"], repeat=2):
    add(f"DATE '{a}' - DATE '{b}'", f"DATE '{a}' < DATE '{b}'", f"DATE '{a}' = DATE '{b}'")
add("DATE '9999-12-31' + 1", "DATE '0001-01-01' - 1", "NULL + DATE '2020-01-01'", "DATE '2020-01-01' + NULL",
    "CAST(DATE '2020-01-01' + 1 AS VARCHAR)", "YEAR(NULL)", "YEAR('1998-12-01')", "DATE '2020-01-01' - NULL")
INTERVALS = [("1", "day"), ("90", "day"), ("-5", "day"), ("1", "week"), ("2", "week"), ("1", "month"), ("3", "month"),
             ("-1", "month"), ("11", "month"), ("12", "month"), ("1", "year"), ("-1", "year"), ("4", "year"),
             ("100", "year"), ("0", "day")]
for d in ["1998-12-01", "2020-01-31", "2020-03-31", "2020-02-29", "2019-02-28", "2023-10-31", "1996-07-15", "2000-02-29"]:
    for n, unit in INTERVALS:
        add(f"CAST(DATE '{d}' + INTERVAL '{n}' {unit} AS DATE)", f"CAST(DATE '{d}' - INTERVAL '{n}' {unit} AS DATE)")
add("CAST(DATE '1998-12-01' - INTERVAL '90' DAY AS DATE) <= DATE '1998-09-02'",
    "CAST(DATE '1993-10-01' + INTERVAL '3' MONTH AS DATE)", "CAST(INTERVAL '5' DAY + DATE '2020-01-01' AS DATE)",
    "CAST(DATE '1994-01-01' + INTERVAL '1' YEAR AS DATE)", "CAST(DATE '1994-01-01' + INTERVAL '3 months' AS DATE)")

# ---------------------------------------------------------------------------- misc
add("-5", "-(-5)", "- -5", "-2147483647", "-(2147483647)", "-CAST(5 AS BIGINT)", "-2.5e0", "-0e0", "-NULL", "-'5'",
    "-CAST('nan' AS DOUBLE)", "-(CAST(-9223372036854775807 AS BIGINT) - 1)", "-DATE '2020-01-01'",
    "1 + 2 * 3", "(1 + 2) * 3", "10 - 4 - 3", "100 / 10 / 5", "7 % 4 * 2", "2 * 3 % 4", "1 + 1 = 2", "1 + 1 = 3 OR TRUE",
    "2147483647 + 1", "2147483647 * 2", "(-2147483647) - 2", "CAST(2147483647 AS BIGINT) + 1",
    "9223372036854775807 + 1", "CAST(9223372036854775807 AS BIGINT) * 2", "5 % 0", "5 / 0", "0 / 0", "-5 / 0e0",
    "CAST(5 AS BIGINT) % 0", "5.5e0 % 2", "-5.5e0 % 2", "5.5e0 % 0", "7 % -3", "-7 % 3", "-7 % -3",
    "CAST(-9223372036854775807 AS BIGINT) % -1", "TRUE + 1", "'a' + 1", "'5' + 1", "DATE '2020-01-01' + DATE '2020-01-01'",
    "DATE '2020-01-01' * 2", "NOT 1", "1 AND 2", "NOT 'a'", "'a' < 1", "TRUE < FALSE", "TRUE = TRUE", "FALSE < TRUE",
    "TRUE <> FALSE", "CAST(TRUE AS INTEGER) + 1", "UPPER(1)", "LENGTH(5)", "ABS('x')", "ROUND('x')",
    "NO_SUCH_FUNCTION(1)", "SUBSTRING('a')", "SUBSTRING('abc', 1.5e0)", "COALESCE()", "ABS(1, 2)")


def duck_type(t: str) -> str:
    return t if t in KNOWN else "OTHER"


def encode(v) -> str:
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if math.isnan(v):
            return "nan"
        if math.isinf(v):
            return "inf" if v > 0 else "-inf"
        return repr(v)
    if isinstance(v, datetime.datetime):
        return v.isoformat()
    if isinstance(v, datetime.date):
        return v.isoformat()
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=False)
    return str(v)


def main():
    con = duckdb.connect()
    seen = set()
    lines = []
    errors = 0
    for e in exprs:
        if e in seen:
            continue
        seen.add(e)
        assert "\t" not in e and "\n" not in e, e
        try:
            value, typ = con.execute(f"SELECT ({e}), typeof({e})").fetchone()
            lines.append(f"{e}\t{duck_type(typ)}\t{encode(value)}")
        except Exception as ex:  # noqa: BLE001 - any DuckDB failure is the expected answer "ERROR"
            errors += 1
            lines.append(f"{e}\tERROR\tERROR")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {len(lines)} expressions ({errors} DuckDB errors) to {OUT}  [duckdb {duckdb.__version__}]")


if __name__ == "__main__":
    main()
