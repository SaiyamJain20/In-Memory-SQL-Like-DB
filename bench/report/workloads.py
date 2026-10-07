"""The workloads of the comparison: what is loaded and which queries run (docs/REPORT.md).

A workload is a load specification (common.py) plus named queries; each query resolves, per
engine, to a file (or to None when the engine cannot express it: reported as N/A, never dropped).

    python3 bench/report/workloads.py gen micro|h2o-g1|h2o-j1 [--rows N]   # writes data/<name>/*.csv
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

DATA = os.path.join(common.ROOT, "data")
SQL_DIR = os.path.join(common.REPORT_DIR, "queries")


class Workload:
    def __init__(self, name, spec, queries, resolver):
        self.name, self.spec, self.queries, self.resolver = name, spec, queries, resolver

    def file(self, engine, query):
        return self.resolver(engine, query)


# ---------------------------------------------------------------------------------- TPC-H

def tpch(sf, query_numbers):
    names = [f"q{q:02d}" for q in query_numbers]

    def resolve(engine, name):
        if engine == "polars":
            return os.path.join(common.REPORT_DIR, "queries", "polars", f"{name}.py")
        return common.query_file(engine, name)
    return Workload(f"tpch-sf{sf:g}", common.tpch_spec(sf), names, resolve)


# ---------------------------------------------------------------------------------- generic SQL sets

def _spec(name, tables):
    d = os.path.join(DATA, name)
    return {"dataset": name, "tables": [
        {"name": t, "path": os.path.join(d, f"{t}.csv"), "columns": cols, "delimiter": "|", "header": False}
        for t, cols in tables]}


def sql_workload(name, tables, catalog, unsupported=None):
    """`catalog`: {query name: SQL}; `unsupported`: {engine: set of query names it cannot run}."""
    unsupported = unsupported or {}
    d = os.path.join(SQL_DIR, name)
    os.makedirs(d, exist_ok=True)
    for q, sql in catalog.items():
        open(os.path.join(d, f"{q}.sql"), "w").write(sql.strip() + "\n")

    def resolve(engine, q):
        if q in unsupported.get(engine, ()):
            return None
        return os.path.join(d, f"{q}.sql")
    return Workload(name, _spec(name, tables), list(catalog), resolve)


MICRO_TABLES = [
    ("t", [("id", "i64"), ("k1k", "i32"), ("k100k", "i32"), ("k1m", "i32"), ("i", "i32"), ("v", "f64"),
           ("s", "str"), ("d", "date")]),
    ("dim1k", [("k", "i32"), ("w", "f64")]),
    ("dim100k", [("k", "i32"), ("w", "f64")]),
    ("dim1m", [("k", "i32"), ("w", "f64")]),
]

MICRO_QUERIES = {
    # scans and expressions
    "scan_count": "SELECT count(*) FROM t",
    "scan_sum": "SELECT sum(v) FROM t",
    "scan_minmax": "SELECT min(v), max(v), min(i), max(i) FROM t",
    "scan_expr": "SELECT sum(v * 2.0 + i) FROM t",
    # filters of rising selectivity (i is uniform on [0, 100000))
    "filter_1pct": "SELECT sum(v) FROM t WHERE i < 1000",
    "filter_10pct": "SELECT sum(v) FROM t WHERE i < 10000",
    "filter_50pct": "SELECT sum(v) FROM t WHERE i < 50000",
    "filter_90pct": "SELECT sum(v) FROM t WHERE i < 90000",
    "filter_and": "SELECT count(*) FROM t WHERE i < 50000 AND v > 500.0 AND k1k < 500",
    "filter_str_eq": "SELECT count(*) FROM t WHERE s = 'str_0042'",
    "filter_like": "SELECT count(*) FROM t WHERE s LIKE '%99%'",
    "filter_date": "SELECT count(*) FROM t WHERE d >= DATE '2018-01-01' AND d < DATE '2019-01-01'",
    # hash aggregation by group cardinality (the high-cardinality results are folded to one row)
    "agg_1k": "SELECT k1k, sum(v), count(*), avg(v) FROM t GROUP BY k1k ORDER BY k1k LIMIT 10",
    "agg_100k": "SELECT count(*), sum(sv), sum(c) FROM (SELECT k100k, sum(v) AS sv, count(*) AS c FROM t GROUP BY k100k) q",
    "agg_1m": "SELECT count(*), sum(sv), sum(c) FROM (SELECT k1m, sum(v) AS sv, count(*) AS c FROM t GROUP BY k1m) q",
    "agg_2keys": "SELECT count(*), sum(sv) FROM (SELECT k1k, d, sum(v) AS sv FROM t GROUP BY k1k, d) q",
    "agg_str": "SELECT s, count(*), avg(v) FROM t GROUP BY s ORDER BY s LIMIT 10",
    "distinct_100k": "SELECT count(DISTINCT k100k) FROM t",
    "distinct_str": "SELECT count(DISTINCT s) FROM t",
    # hash joins by build-side size
    "join_1k": "SELECT sum(t.v * dim1k.w) FROM t JOIN dim1k ON t.k1k = dim1k.k",
    "join_100k": "SELECT sum(t.v * dim100k.w) FROM t JOIN dim100k ON t.k100k = dim100k.k",
    "join_1m": "SELECT sum(t.v * dim1m.w) FROM t JOIN dim1m ON t.k1m = dim1m.k",
    # sorting
    "topn_10": "SELECT id, v FROM t ORDER BY v DESC, id LIMIT 10",
    "sort_5m": "SELECT sum(x) FROM (SELECT v AS x FROM t ORDER BY v LIMIT 5000000) a",
}


def micro():
    return sql_workload("micro", MICRO_TABLES, MICRO_QUERIES)


H2O_G1_TABLES = [("x", [("id1", "str"), ("id2", "str"), ("id3", "str"), ("id4", "i32"), ("id5", "i32"),
                        ("id6", "i32"), ("v1", "i32"), ("v2", "i32"), ("v3", "f64")])]

# H2O.ai db-benchmark "groupby" questions 1-10 (G1_1e7_1e2_0_0). Questions 6 (median, sd), 8 (top 2
# per group) and 9 (corr) need functions cdb does not have: N/A for cdb, run by the others.
H2O_G1_QUERIES = {
    "g1_sum_v1_by_id1": "SELECT id1, sum(v1) AS v1 FROM x GROUP BY id1",
    "g2_sum_v1_by_id1_id2": "SELECT id1, id2, sum(v1) AS v1 FROM x GROUP BY id1, id2",
    "g3_sum_v1_mean_v3_by_id3": "SELECT count(*) AS n, sum(v1) AS v1, sum(v3) AS v3 FROM (SELECT id3, sum(v1) AS v1, avg(v3) AS v3 FROM x GROUP BY id3) q",
    "g4_mean_v1v2v3_by_id4": "SELECT id4, avg(v1) AS v1, avg(v2) AS v2, avg(v3) AS v3 FROM x GROUP BY id4",
    "g5_sum_v1v2v3_by_id6": "SELECT count(*) AS n, sum(v1) AS v1, sum(v2) AS v2, sum(v3) AS v3 FROM (SELECT id6, sum(v1) AS v1, sum(v2) AS v2, sum(v3) AS v3 FROM x GROUP BY id6) q",
    "g6_median_sd_by_id4_id5": "SELECT id4, id5, median(v3) AS median_v3, stddev(v3) AS sd_v3 FROM x GROUP BY id4, id5",
    "g7_range_v1v2_by_id3": "SELECT count(*) AS n, sum(range_v1_v2) AS r FROM (SELECT id3, max(v1) - min(v2) AS range_v1_v2 FROM x GROUP BY id3) q",
    "g8_top2_v3_by_id6": "SELECT id6, v3 AS largest2_v3 FROM (SELECT id6, v3, row_number() OVER (PARTITION BY id6 ORDER BY v3 DESC) AS order_v3 FROM x WHERE v3 IS NOT NULL) sub WHERE order_v3 <= 2",
    "g9_corr_by_id2_id4": "SELECT id2, id4, pow(corr(v1, v2), 2) AS r2 FROM x GROUP BY id2, id4",
    "g10_sum_v3_count_by_id1_6": "SELECT count(*) AS n, sum(v3) AS v3, sum(cnt) AS cnt FROM (SELECT id1, id2, id3, id4, id5, id6, sum(v3) AS v3, count(*) AS cnt FROM x GROUP BY id1, id2, id3, id4, id5, id6) q",
}
# (questions 3, 5, 7 and 10 return up to ~10M rows; their results are folded to one row by an outer
# aggregate that uses every column, so that the clock measures the engine and not the result transfer)


def h2o_g1():
    catalog = dict(H2O_G1_QUERIES)
    na = {"cdb": {"g6_median_sd_by_id4_id5", "g8_top2_v3_by_id6", "g9_corr_by_id2_id4"}}
    return sql_workload("h2o-g1", H2O_G1_TABLES, catalog, na)


H2O_J1_TABLES = [
    ("x", [("id1", "i32"), ("id2", "i32"), ("id3", "i32"), ("id4", "str"), ("id5", "str"), ("id6", "str"),
           ("v1", "f64")]),
    ("small", [("id1", "i32"), ("id4", "str"), ("v2", "f64")]),
    ("medium", [("id1", "i32"), ("id2", "i32"), ("id4", "str"), ("id5", "str"), ("v2", "f64")]),
    ("big", [("id1", "i32"), ("id2", "i32"), ("id3", "i32"), ("id4", "str"), ("id5", "str"), ("id6", "str"),
             ("v2", "f64")]),
]

# H2O.ai db-benchmark "join" questions 1-5 (J1_1e7_NA_0_0); the joined rows are folded to a few
# aggregates (the original benchmark also reduces the result to checks like these).
H2O_J1_QUERIES = {
    "j1_small_inner": "SELECT count(*) AS n, sum(x.v1) AS v1, sum(small.v2) AS v2 FROM x JOIN small ON x.id1 = small.id1",
    "j2_medium_inner": "SELECT count(*) AS n, sum(x.v1) AS v1, sum(medium.v2) AS v2 FROM x JOIN medium ON x.id2 = medium.id2",
    "j3_medium_outer": "SELECT count(*) AS n, sum(x.v1) AS v1, sum(medium.v2) AS v2 FROM x LEFT JOIN medium ON x.id2 = medium.id2",
    "j4_medium_inner_str": "SELECT count(*) AS n, sum(x.v1) AS v1, sum(medium.v2) AS v2 FROM x JOIN medium ON x.id5 = medium.id5",
    "j5_big_inner": "SELECT count(*) AS n, sum(x.v1) AS v1, sum(big.v2) AS v2 FROM x JOIN big ON x.id3 = big.id3",
}


def h2o_j1():
    return sql_workload("h2o-j1", H2O_J1_TABLES, H2O_J1_QUERIES)


def get(name, **kw):
    if name.startswith("tpch"):
        return tpch(kw["sf"], kw["queries"])
    return {"micro": micro, "h2o-g1": h2o_g1, "h2o-j1": h2o_j1}[name]()


# ---------------------------------------------------------------------------------- data generation

def generate(name, rows):
    import duckdb
    d = os.path.join(DATA, name)
    os.makedirs(d, exist_ok=True)
    con = duckdb.connect()
    con.execute("SELECT setseed(0.42)")
    con.execute("SET threads=4")

    def out(table, select):
        con.execute(f"COPY ({select}) TO '{d}/{table}.csv' (DELIMITER '|', HEADER false)")
        print(f"{name}/{table}.csv", flush=True)

    n = rows
    if name == "micro":
        out("t", f"""SELECT i AS id, CAST(random() * 1000 AS INTEGER) AS k1k,
                 CAST(random() * 100000 AS INTEGER) AS k100k, CAST(random() * 1000000 AS INTEGER) AS k1m,
                 CAST(random() * 100000 AS INTEGER) AS i, round(random() * 1000, 2) AS v,
                 'str_' || lpad(CAST(CAST(random() * 1000 AS INTEGER) AS VARCHAR), 4, '0') AS s,
                 DATE '2015-01-01' + CAST(random() * 2556 AS INTEGER) AS d
                 FROM range({n}) r(i)""")
        for tbl, size in (("dim1k", 1000), ("dim100k", 100000), ("dim1m", 1000000)):
            out(tbl, f"SELECT i AS k, round(random() * 10, 3) AS w FROM range({size}) r(i)")
    elif name == "h2o-g1":
        k = 100
        out("x", f"""SELECT 'id' || lpad(CAST(1 + CAST(random() * {k - 1} AS INTEGER) AS VARCHAR), 3, '0') AS id1,
                 'id' || lpad(CAST(1 + CAST(random() * {k - 1} AS INTEGER) AS VARCHAR), 3, '0') AS id2,
                 'id' || lpad(CAST(1 + CAST(random() * {n // k - 1} AS INTEGER) AS VARCHAR), 10, '0') AS id3,
                 1 + CAST(random() * {k - 1} AS INTEGER) AS id4, 1 + CAST(random() * {k - 1} AS INTEGER) AS id5,
                 1 + CAST(random() * {n // k - 1} AS INTEGER) AS id6, 1 + CAST(random() * 4 AS INTEGER) AS v1,
                 1 + CAST(random() * 14 AS INTEGER) AS v2, round(random() * 100, 6) AS v3
                 FROM range({n}) r(i)""")
    elif name == "h2o-j1":
        s, m = max(1, n // 1000000), max(1, n // 1000)
        out("x", f"""SELECT 1 + CAST(random() * {s - 1} AS INTEGER) AS id1, 1 + CAST(random() * {m - 1} AS INTEGER) AS id2,
                 1 + CAST(random() * {n - 1} AS INTEGER) AS id3, 'id' || CAST(1 + CAST(random() * {s - 1} AS INTEGER) AS VARCHAR) AS id4,
                 'id' || CAST(1 + CAST(random() * {m - 1} AS INTEGER) AS VARCHAR) AS id5,
                 'id' || CAST(1 + CAST(random() * {n - 1} AS INTEGER) AS VARCHAR) AS id6, round(random() * 100, 6) AS v1
                 FROM range({n}) r(i)""")
        out("small", f"SELECT i + 1 AS id1, 'id' || CAST(i + 1 AS VARCHAR) AS id4, round(random() * 100, 6) AS v2 FROM range({s}) r(i)")
        out("medium", f"""SELECT 1 + CAST(random() * {s - 1} AS INTEGER) AS id1, i + 1 AS id2,
                 'id' || CAST(1 + CAST(random() * {s - 1} AS INTEGER) AS VARCHAR) AS id4, 'id' || CAST(i + 1 AS VARCHAR) AS id5,
                 round(random() * 100, 6) AS v2 FROM range({m}) r(i)""")
        out("big", f"""SELECT 1 + CAST(random() * {s - 1} AS INTEGER) AS id1, 1 + CAST(random() * {m - 1} AS INTEGER) AS id2,
                 i + 1 AS id3, 'id' || CAST(1 + CAST(random() * {s - 1} AS INTEGER) AS VARCHAR) AS id4,
                 'id' || CAST(1 + CAST(random() * {m - 1} AS INTEGER) AS VARCHAR) AS id5,
                 'id' || CAST(i + 1 AS VARCHAR) AS id6, round(random() * 100, 6) AS v2 FROM range({n}) r(i)""")
    else:
        raise SystemExit(f"unknown dataset {name}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("name", choices=["micro", "h2o-g1", "h2o-j1"])
    g.add_argument("--rows", type=int, default=10_000_000)
    args = ap.parse_args()
    generate(args.name, args.rows)


if __name__ == "__main__":
    main()
