#!/usr/bin/env python3
"""How good are the optimizer's numbers?  Two measurements on TPC-H data (tools/tpch_data.py):

    .venv/bin/python tools/stats_accuracy.py ndv --sf 1        # sketch distinct counts vs DuckDB's exact counts
    .venv/bin/python tools/stats_accuracy.py estimates --sf 1  # estimated vs actual rows of every operator

Both load the CSV files into cdb_shell (build/release/tools/cdb_shell by default) with COPY and read
what the engine reports: `.stats` for the first, `EXPLAIN ANALYZE` of the 22 queries for the second.

The q-error of an estimate is max(est / actual, actual / est) with both clamped to at least 1: 1.0 is
exact, 2.0 is off by a factor of two in either direction.
"""
import argparse
import math
import os
import re
import statistics
import subprocess
import sys

import duckdb

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLES = ["region", "nation", "supplier", "customer", "part", "partsupp", "orders", "lineitem"]


def load_script(sf):
    data = os.path.join(ROOT, "data", f"tpch-sf{sf:g}")
    out = [open(os.path.join(ROOT, "bench", "tpch", "schema.sql")).read()]
    for t in TABLES:
        out.append(f"COPY {t} FROM '{data}/{t}.csv' (DELIMITER '|', HEADER FALSE);")
    return "\n".join(out) + "\n"


def run_shell(shell, script):
    # (a blank line starts a statement in the shell's buffer, and a dot command only runs when the
    # buffer is empty: no blank lines)
    script = "\n".join(line for line in script.splitlines() if line.strip()) + "\n"
    res = subprocess.run([shell], input=script, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"cdb_shell failed: {res.stderr[:500]}")
    return res.stdout


def ndv(args):
    sf = args.sf
    out = run_shell(args.shell, load_script(sf) + "\n.stats\n")
    reported = {}  # table.column -> (nulls, distinct)
    for line in out.splitlines():
        line = re.sub(r"^(cdb> |\.\.\.> )+", "", line)
        m = re.match(r"^(\w+\.\w+)\t(\d+) nulls\t(\d+) distinct", line)
        if m:
            reported[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    con = duckdb.connect()
    con.execute("INSTALL tpch; LOAD tpch;")
    con.execute(f"CALL dbgen(sf={sf})")
    rows = []
    for key, (_nulls, est) in sorted(reported.items()):
        table, col = key.split(".")
        exact = con.execute(f"SELECT count(DISTINCT {col}) FROM {table}").fetchone()[0]
        rows.append((key, exact, est))
    errs = []
    print(f"{'column':<22}{'exact':>10}{'sketch':>10}{'error':>9}")
    for key, exact, est in rows:
        err = (est - exact) / exact if exact else 0.0
        errs.append(abs(err))
        print(f"{key:<22}{exact:>10}{est:>10}{err:>+9.1%}")
    big = [e for (k, ex, es), e in zip(rows, errs) if ex >= 1000]
    print(f"\n{len(rows)} columns: median |error| {statistics.median(errs):.2%}, "
          f"max {max(errs):.2%}; columns with >= 1000 distinct values ({len(big)}): "
          f"median {statistics.median(big):.2%}, max {max(big):.2%}")


LINE = re.compile(r"^( *)(.*?)  \(est ~(\d+), actual (\d+) rows, ([0-9.]+) ms")


def qerror(est, actual):
    est, actual = max(1, est), max(1, actual)
    return max(est / actual, actual / est)


def estimates(args):
    sf = args.sf
    queries = []
    for q in range(1, 23):
        sql = open(os.path.join(ROOT, "bench", "tpch", "queries", f"q{q:02d}.sql")).read().strip().rstrip(";")
        queries.append((q, sql))
    script = load_script(sf) + "\n.threads 1\n" + "\n".join(f"EXPLAIN ANALYZE {sql};" for _, sql in queries) + "\n"
    out = run_shell(args.shell, script)
    # split the output into one block per EXPLAIN ANALYZE result
    blocks, current = [], None
    for line in out.splitlines():
        line = re.sub(r"^(cdb> |\.\.\.> )+", "", line)
        if line.startswith(" explain_value"):
            current = []
            blocks.append(current)
        elif current is not None:
            current.append(line)
    blocks = blocks[-22:]
    if len(blocks) != 22:
        sys.exit(f"expected 22 EXPLAIN ANALYZE results, found {len(blocks)}")
    kinds = {"SCAN": [], "FILTER": [], "JOIN": [], "AGGREGATE": [], "other": []}
    per_query = []
    everything = []
    for (q, _), block in zip(queries, blocks):
        qerrs = []
        for line in block:
            m = LINE.match(" " + line if not line.startswith(" ") else line)
            if not m:
                continue
            op = m.group(2)
            est, actual = int(m.group(3)), int(m.group(4))
            e = qerror(est, actual)
            qerrs.append(e)
            everything.append((e, q, op, est, actual))
            key = next((k for k in kinds if op.startswith(k)), "other")
            kinds[key].append(e)
        per_query.append((q, qerrs))
    print(f"{'query':<7}{'operators':>10}{'median q-error':>16}{'max q-error':>13}")
    for q, e in per_query:
        print(f"Q{q:<6}{len(e):>10}{statistics.median(e):>16.2f}{max(e):>13.1f}")

    def pct(values, p):
        s = sorted(values)
        return s[min(len(s) - 1, math.ceil(p * len(s)) - 1)]

    print(f"\n{'operator kind':<14}{'count':>7}{'median':>9}{'90th pct':>10}{'max':>9}")
    for k, v in kinds.items():
        if v:
            print(f"{k:<14}{len(v):>7}{statistics.median(v):>9.2f}{pct(v, 0.9):>10.2f}{max(v):>9.1f}")
    allq = [e for e, *_ in everything]
    print(f"{'all':<14}{len(allq):>7}{statistics.median(allq):>9.2f}{pct(allq, 0.9):>10.2f}{max(allq):>9.1f}")
    print("\nworst operators:")
    for e, q, op, est, actual in sorted(everything, reverse=True)[:int(os.environ.get("TOP", "8"))]:
        print(f"  Q{q:<3} q-error {e:>9.1f}  est {est:>9}  actual {actual:>9}  {op[:90]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["ndv", "estimates"])
    ap.add_argument("--sf", type=float, default=1)
    ap.add_argument("--shell", default=os.path.join(ROOT, "build", "release", "tools", "cdb_shell"))
    args = ap.parse_args()
    {"ndv": ndv, "estimates": estimates}[args.what](args)


if __name__ == "__main__":
    main()
