#!/usr/bin/env python3
"""Fills in (or refreshes) the expected results of sqllogictest-style files with DuckDB's answers.

    .venv/bin/python tools/gen_slt.py tests/sql/*.test

The files hold the SQL; DuckDB is the oracle for what it must return. Format (a subset of
sqllogictest, tab-separated like DuckDB's own test files):

    # comment
    statement ok                 <- SQL (until a blank line) must succeed
    CREATE TABLE t (a INTEGER);

    statement error              <- must fail
    SELECT nope;

    query IT rowsort             <- column types (I integer, T text, R real) and an optional sort
    SELECT a, b FROM t;
    ----
    1<TAB>x

A query's rows are rewritten from DuckDB on every run. Rendering is shared with the C++ runner
(tests/sql/slt_runner_test.cpp): NULL -> NULL, booleans true/false, empty string -> (empty),
doubles as Python repr (nan, inf), dates ISO. `rowsort` sorts the rendered lines bytewise.
Queries must stay inside what both engines agree on (docs/adr/0003): no DECIMAL arithmetic,
no non-ASCII case mapping.
"""
import datetime
import math
import sys

import duckdb


def render(v):
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        if math.isnan(v):
            return "nan"
        if math.isinf(v):
            return "inf" if v > 0 else "-inf"
        return repr(v)
    if isinstance(v, datetime.date):
        return v.isoformat()
    if isinstance(v, str):
        return v if v != "" else "(empty)"
    return str(v)


def parse(text):
    """Yields ('comment'|'blank', line) or (kind, header, sql, results) blocks."""
    lines = text.split("\n")
    i = 0
    out = []
    while i < len(lines):
        line = lines[i]
        if line.startswith("statement ") or line.startswith("query "):
            header = line
            i += 1
            sql = []
            while i < len(lines) and lines[i] != "" and lines[i] != "----":
                sql.append(lines[i])
                i += 1
            results = None
            if i < len(lines) and lines[i] == "----":
                i += 1
                results = []
                while i < len(lines) and lines[i] != "":
                    results.append(lines[i])
                    i += 1
            out.append(("block", header, "\n".join(sql), results))
        else:
            out.append(("text", line))
            i += 1
    return out


def process(path):
    con = duckdb.connect()
    blocks = parse(open(path).read())
    out = []
    for b in blocks:
        if b[0] == "text":
            out.append(b[1])
            continue
        _, header, sql, _results = b
        words = header.split()
        if words[0] == "statement":
            try:
                con.execute(sql)
                failed = False
            except Exception as e:  # noqa: BLE001
                failed = True
                err = e
            if (words[1] == "ok") == failed:
                raise SystemExit(f"{path}: statement expected '{words[1]}' but DuckDB "
                                 f"{'failed: ' + str(err) if failed else 'succeeded'}:\n{sql}")
            out.append(header)
            out.append(sql)
        else:
            try:
                rows = con.execute(sql).fetchall()
            except Exception as e:  # noqa: BLE001
                raise SystemExit(f"{path}: query failed in DuckDB: {e}\n{sql}")
            lines = ["\t".join(render(v) for v in r) for r in rows]
            if "rowsort" in words[2:]:
                lines.sort()
            ncols = len(rows[0]) if rows else len(words[1]) if len(words) > 1 else 0
            if rows and len(words[1]) != ncols:
                raise SystemExit(f"{path}: header {header!r} lists {len(words[1])} columns, "
                                 f"query returns {ncols}:\n{sql}")
            out.append(header)
            out.append(sql)
            out.append("----")
            out.extend(lines)
    open(path, "w").write("\n".join(out))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        process(p)
        print("updated", p)
