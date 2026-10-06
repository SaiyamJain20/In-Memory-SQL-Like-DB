#!/usr/bin/env python3
"""Generates TPC-H data and DuckDB's reference answers for the differential tests and benchmarks.

    .venv/bin/python tools/tpch_data.py --sf 0.01 [--out data/tpch-sf0.01]

Writes into <out>:
    <table>.csv           the 8 TPC-H tables, '|' delimited, no header (loadable with COPY ... (DELIMITER '|'))
    expected/qNN.csv      DuckDB's result of bench/tpch/queries/qNN.sql for every query this engine can run
    manifest.json         scale factor, row counts, DuckDB version, which queries have answers

The queries are the exact SQL text in bench/tpch/queries (exported from DuckDB), run by DuckDB on
the same data, so a mismatch is a bug in this engine rather than in the harness.
"""
import argparse
import json
import pathlib

import duckdb

ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLES = ["nation", "region", "part", "supplier", "partsupp", "customer", "orders", "lineitem"]
# All 22 TPC-H queries (subqueries and WITH are supported since Phase 8).
SUPPORTED = list(range(1, 23))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=float, default=0.01)
    ap.add_argument("--out", default=None)
    ap.add_argument("--queries", default=",".join(map(str, SUPPORTED)),
                    help="comma-separated query numbers to compute answers for")
    args = ap.parse_args()
    out = pathlib.Path(args.out) if args.out else ROOT / "data" / f"tpch-sf{args.sf:g}"
    (out / "expected").mkdir(parents=True, exist_ok=True)

    con = duckdb.connect()
    con.execute("INSTALL tpch; LOAD tpch;")
    con.execute(f"CALL dbgen(sf={args.sf})")
    counts = {}
    for t in TABLES:
        counts[t] = con.execute(f"SELECT count(*) FROM {t}").fetchone()[0]
        con.execute(f"COPY {t} TO '{out / (t + '.csv')}' (DELIMITER '|', HEADER false)")
    answered = []
    for q in [int(x) for x in args.queries.split(",") if x]:
        sql = (ROOT / "bench" / "tpch" / "queries" / f"q{q:02d}.sql").read_text().strip().rstrip(";")
        con.execute(f"COPY ({sql}) TO '{out / 'expected' / f'q{q:02d}.csv'}' (DELIMITER '|', HEADER false)")
        answered.append(q)
    (out / "manifest.json").write_text(json.dumps({
        "scale_factor": args.sf, "row_counts": counts, "duckdb": duckdb.__version__, "queries": answered,
    }, indent=2))
    print(f"wrote TPC-H SF{args.sf:g} to {out}: " + ", ".join(f"{t}={n}" for t, n in counts.items()))


if __name__ == "__main__":
    main()
