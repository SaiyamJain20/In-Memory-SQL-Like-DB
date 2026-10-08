#!/usr/bin/env python3
"""How good are the optimizers' numbers and plans? cdb against DuckDB on the 22 TPC-H queries.

    optimizer_quality.py --sf 1 --out results/optimizer_quality_sf1.json

For every query both engines run once on one thread with profiling on and report, per operator,
the rows the optimizer expected and the rows that came out:
  * cdb: EXPLAIN ANALYZE  ("(est ~E, actual A rows, T ms ...)" on every operator line);
  * DuckDB: PRAGMA enable_profiling='json' (operator_cardinality and "Estimated Cardinality").
From these: the q-error max(est/actual, actual/est) (both clamped to >= 1) per operator, by kind,
and C_out, the rows produced by all joins together - a plan-quality measure that does not depend on
the speed of an engine's operators (the smaller, the less intermediate data the join order made).
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

LINE = re.compile(r"^(\s*)(.*?)\s+\(est ~(\d+), actual (\d+) rows")


def kind_cdb(op):
    for k in ("SCAN", "FILTER", "JOIN", "AGGREGATE"):
        if op.startswith(k):
            return k
    return "OTHER"


def kind_duckdb(op):
    if "SCAN" in op:
        return "SCAN"
    if op == "FILTER":
        return "FILTER"
    if "JOIN" in op or op in ("CROSS_PRODUCT", "ASOF_JOIN"):
        return "JOIN"
    if "GROUP_BY" in op or "AGGREGATE" in op:
        return "AGGREGATE"
    return "OTHER"


def cdb_plans(sf, threads, queries):
    spec = common.tpch_spec(sf)
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        w = driver.Worker("cdb", threads, sf)
        try:
            driver.load_tables(w, spec, tmp)
            for q in queries:
                name = f"q{q:02d}"
                sql = open(common.query_file("cdb", name)).read().strip().rstrip(";")
                src = os.path.join(tmp, f"{name}.sql")
                open(src, "w").write("EXPLAIN ANALYZE " + sql)
                dump = os.path.join(tmp, f"{name}.csv")
                w.call("DUMP", src, dump, timeout=600)
                ops = []
                for row in common.read_pipe_csv(open(dump).read()):
                    m = LINE.match(row[0])
                    if m:
                        op = m.group(2)
                        ops.append({"op": op[:60], "kind": kind_cdb(op), "est": int(m.group(3)),
                                    "actual": int(m.group(4))})
                out[name] = ops
        finally:
            w.close()
    return out


def duckdb_plans(sf, threads, queries):
    import duckdb
    spec = common.tpch_spec(sf)
    con = duckdb.connect(":memory:")
    con.execute(f"SET threads={threads}")
    for t in spec["tables"]:
        cols = ", ".join(f"{c} {'INTEGER' if ty == 'i32' else 'DOUBLE' if ty == 'f64' else 'DATE' if ty == 'date' else 'VARCHAR'} NOT NULL"
                         for c, ty in t["columns"])
        con.execute(f"CREATE TABLE {t['name']} ({cols})")
        con.execute(f"COPY {t['name']} FROM '{t['path']}' (DELIMITER '|', HEADER false)")
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "profile.json")
        con.execute("PRAGMA enable_profiling='json'")
        con.execute(f"PRAGMA profiling_output='{path}'")
        for q in queries:
            name = f"q{q:02d}"
            sql = open(common.query_file("duckdb", name)).read().strip().rstrip(";")
            con.execute(sql).fetchall()
            tree = json.load(open(path))
            ops = []

            def walk(n):
                est = n.get("extra_info", {}).get("Estimated Cardinality")
                if n.get("operator_type") and est is not None:
                    ops.append({"op": n["operator_type"], "kind": kind_duckdb(n["operator_type"]),
                                "est": int(str(est).replace(",", "").lstrip("~")),
                                "actual": int(n.get("operator_cardinality", 0))})
                for c in n.get("children", []):
                    walk(c)
            for c in tree.get("children", []):
                walk(c)
            out[name] = ops
    return out


def qerror(est, actual):
    est, actual = max(1, est), max(1, actual)
    return max(est / actual, actual / est)


def summarise(plans):
    kinds = {}
    c_out = {}
    for name, ops in plans.items():
        c_out[name] = sum(o["actual"] for o in ops if o["kind"] == "JOIN")
        for o in ops:
            kinds.setdefault(o["kind"], []).append(qerror(o["est"], o["actual"]))
    rows = {}
    for k, v in sorted(kinds.items()):
        rows[k] = {"n": len(v), "median": common.median(v), "p90": common.quantile(v, 0.9), "max": max(v)}
    allq = [x for v in kinds.values() for x in v]
    rows["ALL"] = {"n": len(allq), "median": common.median(allq), "p90": common.quantile(allq, 0.9),
                   "max": max(allq)}
    return rows, c_out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=float, default=1)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    queries = list(range(1, 23))
    result = {"sf": args.sf, "threads": args.threads, "engines": {}}
    for engine, fn in (("cdb", cdb_plans), ("duckdb", duckdb_plans)):
        plans = fn(args.sf, args.threads, queries)
        summary, c_out = summarise(plans)
        result["engines"][engine] = {"plans": plans, "qerror": summary, "c_out": c_out}
        print(engine, {k: f"{v['median']:.2f}/{v['p90']:.1f}/{v['max']:.0f}" for k, v in summary.items()}, flush=True)
    json.dump(result, open(args.out, "w"), indent=1)


if __name__ == "__main__":
    main()
