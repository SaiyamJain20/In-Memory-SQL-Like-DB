#!/usr/bin/env python3
"""Rewrites the DuckDB-exported TPC-H queries (bench/tpch/queries) for SQLite: dates are ISO text,
EXTRACT and SUBSTRING FROM FOR become strftime / substr. Everything else is left as it is.

    python3 bench/report/make_sqlite_queries.py     # writes bench/report/queries/sqlite/qNN.sql
"""
import glob
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(os.path.dirname(HERE)), "bench", "tpch", "queries")
OUT = os.path.join(HERE, "queries", "sqlite")


def translate(sql):
    sql = re.sub(r"extract\(\s*year\s+FROM\s+([\w.]+)\s*\)", r"CAST(strftime('%Y', \1) AS INTEGER)", sql,
                 flags=re.I)
    sql = re.sub(r"substring\(\s*([\w.]+)\s+FROM\s+(\d+)\s+FOR\s+(\d+)\s*\)", r"substr(\1, \2, \3)", sql,
                 flags=re.I)
    sql = re.sub(r"CAST\('(\d{4}-\d{2}-\d{2})'\s+AS\s+date\)", r"'\1'", sql, flags=re.I)
    sql = re.sub(r"\bdate\s+'(\d{4}-\d{2}-\d{2})'", r"'\1'", sql, flags=re.I)
    # Q13: SQLite has no column alias list on a derived table
    sql = re.sub(r"count\(o_orderkey\)(\s+FROM\s+customer)", r"count(o_orderkey) AS c_count\1", sql)
    sql = re.sub(r"\)\s+AS\s+c_orders\s*\(\s*c_custkey\s*,\s*c_count\s*\)", ") AS c_orders", sql)
    return sql


def main():
    os.makedirs(OUT, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(SRC, "q*.sql"))):
        text = open(path).read()
        out = translate(text)
        open(os.path.join(OUT, os.path.basename(path)), "w").write(out)
        changed = "rewritten" if out != text else "unchanged"
        print(os.path.basename(path), changed)


if __name__ == "__main__":
    main()
