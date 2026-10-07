#!/usr/bin/env python3
"""The worked example of the report: one TPC-H query's SQL, plan and EXPLAIN ANALYZE in cdb.

    explain_examples.py --sf 1 --query 3 --out docs/report/generated/explain_q03.md
"""
import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
import driver  # noqa: E402


def lines_of(w, sql, tmp, name):
    src = os.path.join(tmp, name + ".sql")
    open(src, "w").write(sql)
    out = os.path.join(tmp, name + ".csv")
    w.call("DUMP", src, out, timeout=600)
    return [r[0] for r in common.read_pipe_csv(open(out).read())]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=float, default=1)
    ap.add_argument("--query", type=int, default=3)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    name = f"q{args.query:02d}"
    sql = open(common.query_file("cdb", name)).read().strip().rstrip(";")
    with tempfile.TemporaryDirectory() as tmp:
        w = driver.Worker("cdb", args.threads, args.sf)
        try:
            driver.load_tables(w, common.tpch_spec(args.sf), tmp)
            plan = lines_of(w, "EXPLAIN " + sql, tmp, "plan")
            for _ in range(2):  # warm
                lines_of(w, sql, tmp, "run")
            analyzed = lines_of(w, "EXPLAIN ANALYZE " + sql, tmp, "analyze")
        finally:
            w.close()

    def strip(line):  # the plan without the estimate suffix, for the logical-plan listing
        return re.sub(r"\s+\(~\d+ rows\)\s*$", "", line)
    text = [f"The query (TPC-H {name.upper()}, shipping priority) joins three tables, filters two of them, groups, sorts and "
            "keeps the ten best groups:", "", "```sql", sql, "```", "",
            f"After parsing, binding and optimizing, cdb prints the plan with the optimizer's row estimates (`EXPLAIN`, SF{args.sf:g}):",
            "", "```", *[l.rstrip() for l in plan], "```", "",
            f"`EXPLAIN ANALYZE` runs the query (here {args.threads} thread{'s' if args.threads > 1 else ''}, warm) and adds the rows that "
            "actually came out of every operator, the number of rows each join built its hash table from, the rows each sink consumed "
            "and the CPU time of each operator's own calls:", "", "```", *[l.rstrip() for l in analyzed], "```", ""]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "w").write("\n".join(text))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
