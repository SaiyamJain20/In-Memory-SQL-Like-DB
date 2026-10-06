#!/usr/bin/env python3
"""Times the TPC-H queries in DuckDB on this machine, for an honest side-by-side with cdb_tpch.

    .venv/bin/python tools/tpch_duckdb_time.py --sf 1 [--threads 1] [--runs 5] [--queries 1,3,6]

DuckDB generates the data itself (CALL dbgen) in its own in-memory storage, then each query from
bench/tpch/queries is run `runs` times; the first run is "cold", min/median are over the rest.
`--threads 1` pins DuckDB to one thread, the fair comparison until this engine is parallel (Phase 6).
"""
import argparse
import pathlib
import statistics
import time

import duckdb

ROOT = pathlib.Path(__file__).resolve().parent.parent
SUPPORTED = [1, 3, 5, 6, 7, 8, 9, 10, 12, 13, 14, 19]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=float, default=0.1)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--queries", default=",".join(map(str, SUPPORTED)))
    args = ap.parse_args()

    con = duckdb.connect()
    con.execute("INSTALL tpch; LOAD tpch;")
    t0 = time.perf_counter()
    con.execute(f"CALL dbgen(sf={args.sf})")
    print(f"DuckDB {duckdb.__version__}, SF{args.sf:g}, threads={args.threads}; dbgen took "
          f"{time.perf_counter() - t0:.2f} s")
    con.execute(f"PRAGMA threads={args.threads}")
    print(f"{'q':<4} {'rows':>8} {'cold ms':>10} {'min ms':>10} {'median ms':>10}")
    for q in [int(x) for x in args.queries.split(",") if x]:
        sql = (ROOT / "bench" / "tpch" / "queries" / f"q{q:02d}.sql").read_text().strip().rstrip(";")
        times, rows = [], 0
        for _ in range(args.runs):
            t = time.perf_counter()
            rows = len(con.execute(sql).fetchall())
            times.append((time.perf_counter() - t) * 1000)
        rest = sorted(times[1:] if len(times) > 1 else times)
        print(f"{q:<4} {rows:>8} {times[0]:>10.1f} {rest[0]:>10.1f} {statistics.median(rest):>10.1f}")


if __name__ == "__main__":
    main()
