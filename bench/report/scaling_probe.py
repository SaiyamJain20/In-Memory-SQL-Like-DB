#!/usr/bin/env python3
"""Why a query does not scale: EXPLAIN ANALYZE of one TPC-H query on 1 and on 16 threads, with the
CPU the process used (getrusage) next to the CPU the operators account for.

    scaling_probe.py --query 4 --sf 1 --out results/scaling_q04.json
"""
import argparse
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
import driver  # noqa: E402

LINE = re.compile(r"\(est ~(\d+), actual (\d+) rows, ([0-9.]+) ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", type=int, default=4)
    ap.add_argument("--sf", type=float, default=1)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    result = {}
    for t in (1, 16):
        with tempfile.TemporaryDirectory() as tmp:
            w = driver.Worker("cdb", t, args.sf)
            try:
                driver.load_tables(w, common.tpch_spec(args.sf), tmp)
                sql = open(common.query_file("cdb", f"q{args.query:02d}")).read().strip().rstrip(";")
                f = os.path.join(tmp, "q.sql")
                open(f, "w").write(sql)
                r = w.call("RUN", f, 5)
                open(f, "w").write("EXPLAIN ANALYZE " + sql)
                out = os.path.join(tmp, "o.csv")
                w.call("DUMP", f, out)
                text = [row[0] for row in common.read_pipe_csv(open(out).read())]
            finally:
                w.close()
        operators = sum(float(m.group(3)) for line in text for m in [LINE.search(line)] if m)
        result[t] = {"wall_ms": common.median(r["wall_ms"][1:]), "process_cpu_ms": common.median(r["cpu_ms"][1:]),
                     "operator_own_cpu_ms": operators, "explain_analyze": text}
        print(t, {k: round(v, 1) for k, v in result[t].items() if k != "explain_analyze"})
    json.dump(result, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    main()
