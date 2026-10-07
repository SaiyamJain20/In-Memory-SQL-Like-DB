#!/usr/bin/env python3
"""Durability and ingest: what keeping data on disk costs each engine (docs/REPORT.md).

    durability.py --sf 1 --commits 2000 --out results/durability.json

Per engine, on the machine's NVMe disk (ext4, page cache warm for reopening):
  * commit latency of single-row INSERT statements in autocommit mode, with the engine's durable
    setting (fsync before the statement returns) and with durability off;
  * bulk load of TPC-H into a persistent database (wall seconds) and the bytes it occupies;
  * reopen: seconds from opening the files to the answer of TPC-H Q6.
cdb is measured by bench/persist/persist_bench.cpp (cdb_persist), the others here. Polars and
DataFusion have no storage of their own and are not in this table. The engines are different kinds
of system (an OLTP-style store, two analytical stores, an analytical engine with a log): the
numbers say what each design costs, not which is better.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

ROOT = common.ROOT
SCRATCH = os.path.join(ROOT, "build", "durability_scratch")
Q6 = ("SELECT sum(l_extendedprice * l_discount) FROM lineitem WHERE l_shipdate >= '1994-01-01' "
      "AND l_shipdate < '1995-01-01' AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24")


def dir_size(path):
    total = 0
    for base, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(base, f))
            except OSError:
                pass
    return total


def latency_stats(ms):
    s = sorted(ms)
    return {"n": len(s), "mean_ms": statistics.mean(s), "p50_ms": common.quantile(s, 0.5),
            "p99_ms": common.quantile(s, 0.99), "max_ms": s[-1], "per_s": 1000.0 / statistics.mean(s)}


def fresh(name):
    path = os.path.join(SCRATCH, name)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path)
    return path


# ---------------------------------------------------------------------------------- SQLite

def sqlite_run(sf, commits):
    import sqlite3
    out = {}
    spec = common.tpch_spec(sf)
    for label, sync in (("durable", "FULL"), ("not durable", "OFF")):
        path = os.path.join(fresh(f"sqlite_{sync}"), "db.sqlite")
        con = sqlite3.connect(path, isolation_level=None)
        con.execute("PRAGMA journal_mode=WAL")
        con.execute(f"PRAGMA synchronous={sync}")
        con.execute("CREATE TABLE t (a INTEGER, b TEXT, c REAL)")
        ms = []
        for i in range(commits):
            t = time.perf_counter()
            con.execute("INSERT INTO t VALUES (?, ?, ?)", (i, "x", 1.5))  # autocommit: one transaction
            ms.append((time.perf_counter() - t) * 1000)
        out[f"commit ({label})"] = latency_stats(ms)
        con.close()
    # bulk load, durable, one transaction per table
    path = os.path.join(fresh("sqlite_bulk"), "db.sqlite")
    import csv
    con = sqlite3.connect(path, isolation_level=None)
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA synchronous=FULL")
    types = {"i32": "INTEGER", "i64": "INTEGER", "f64": "REAL", "date": "TEXT", "str": "TEXT"}
    t0 = time.perf_counter()
    for t in spec["tables"]:
        con.execute(f"CREATE TABLE {t['name']} ({', '.join(f'{c} {types[ty]} NOT NULL' for c, ty in t['columns'])})")
        conv = [int if ty in ('i32', 'i64') else float if ty == 'f64' else None for _, ty in t["columns"]]
        con.execute("BEGIN")
        with open(t["path"], newline="") as f:
            con.executemany(f"INSERT INTO {t['name']} VALUES ({','.join('?' * len(conv))})",
                            ([c(v) if c else v for c, v in zip(conv, r)] for r in csv.reader(f, delimiter="|")))
        con.execute("COMMIT")
    con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    out["bulk load"] = {"s": time.perf_counter() - t0}
    con.close()
    out["bulk load"]["bytes"] = dir_size(os.path.dirname(path))
    t = time.perf_counter()
    con = sqlite3.connect(path)
    con.execute(Q6).fetchall()
    out["reopen"] = {"s": time.perf_counter() - t}
    con.close()
    return out


# ---------------------------------------------------------------------------------- DuckDB

def duckdb_run(sf, commits):
    import duckdb
    out = {}
    spec = common.tpch_spec(sf)
    path = os.path.join(fresh("duckdb_commit"), "db.duckdb")
    con = duckdb.connect(path)
    con.execute("CREATE TABLE t (a INTEGER, b VARCHAR, c DOUBLE)")
    ms = []
    for i in range(commits):
        t = time.perf_counter()
        con.execute("INSERT INTO t VALUES (?, ?, ?)", [i, "x", 1.5])  # autocommit, WAL flushed per commit
        ms.append((time.perf_counter() - t) * 1000)
    out["commit (durable)"] = latency_stats(ms)
    con.close()
    types = {"i32": "INTEGER", "i64": "BIGINT", "f64": "DOUBLE", "date": "DATE", "str": "VARCHAR"}
    path = os.path.join(fresh("duckdb_bulk"), "db.duckdb")
    con = duckdb.connect(path)
    t0 = time.perf_counter()
    for t in spec["tables"]:
        con.execute(f"CREATE TABLE {t['name']} ({', '.join(f'{c} {types[ty]} NOT NULL' for c, ty in t['columns'])})")
        con.execute(f"COPY {t['name']} FROM '{t['path']}' (DELIMITER '|', HEADER false)")
    con.execute("CHECKPOINT")
    out["bulk load"] = {"s": time.perf_counter() - t0}
    con.close()
    out["bulk load"]["bytes"] = dir_size(os.path.dirname(path))
    t = time.perf_counter()
    con = duckdb.connect(path)
    con.execute(Q6).fetchall()
    out["reopen"] = {"s": time.perf_counter() - t}
    con.close()
    return out


# ---------------------------------------------------------------------------------- ClickHouse

def chdb_run(sf, commits):
    from chdb import session
    out = {}
    spec = common.tpch_spec(sf)
    types = {"i32": "Int32", "i64": "Int64", "f64": "Float64", "date": "Date", "str": "String"}
    for label, settings in (("durable", "fsync_after_insert = 1, fsync_part_directory = 1"),
                            ("not durable", "")):
        sess = session.Session(fresh(f"chdb_commit_{'d' if settings else 'n'}"))
        sess.query("SET join_use_nulls = 1")
        sess.query("CREATE TABLE t (a Int32, b String, c Float64) ENGINE = MergeTree ORDER BY a"
                   + (f" SETTINGS {settings}" if settings else ""))
        ms = []
        for i in range(commits // 4):  # every INSERT creates a part directory: far slower than a log append
            t = time.perf_counter()
            sess.query(f"INSERT INTO t VALUES ({i}, 'x', 1.5)")
            ms.append((time.perf_counter() - t) * 1000)
        out[f"commit ({label})"] = latency_stats(ms)
        sess.close()
    path = fresh("chdb_bulk")
    sess = session.Session(path)
    t0 = time.perf_counter()
    for t in spec["tables"]:
        cols = ", ".join(f"{c} {types[ty]}" for c, ty in t["columns"])
        sess.query(f"CREATE TABLE {t['name']} ({cols}) ENGINE = MergeTree ORDER BY tuple()")
        sess.query(f"INSERT INTO {t['name']} SELECT * FROM file('{t['path']}', 'CSV', '{cols}') "
                   f"SETTINGS format_csv_delimiter = '|', input_format_csv_trim_whitespaces = 0")
    sess.query("OPTIMIZE TABLE lineitem FINAL")
    out["bulk load"] = {"s": time.perf_counter() - t0}
    sess.close()
    out["bulk load"]["bytes"] = dir_size(path)
    t = time.perf_counter()
    sess = session.Session(path)
    sess.query(Q6.replace("l_shipdate >= '1994-01-01'", "l_shipdate >= toDate('1994-01-01')")
               .replace("l_shipdate < '1995-01-01'", "l_shipdate < toDate('1995-01-01')"), "CSV")
    out["reopen"] = {"s": time.perf_counter() - t}
    sess.close()
    return out


# ---------------------------------------------------------------------------------- cdb

def cdb_run(sf, threads):
    exe = os.path.join(ROOT, "build", "release", "bench", "cdb_persist")
    scratch = fresh("cdb_persist")
    text = subprocess.run([exe, "--dir", scratch, "--sf", f"{sf:g}", "--threads", str(threads)],
                          capture_output=True, text=True, timeout=3600).stdout
    out = {"raw": text}

    def grab(pattern):
        m = re.search(pattern, text)
        return m.groups() if m else None
    m = grab(r"fsync per statement \(Full\)\s+(\d+) stmts/s\s+mean\s+([\d.]+) us\s+p50\s+([\d.]+)\s+p99\s+([\d.]+)\s+max\s+([\d.]+)")
    if m:
        out["commit (durable)"] = {"per_s": float(m[0]), "mean_ms": float(m[1]) / 1000, "p50_ms": float(m[2]) / 1000,
                                   "p99_ms": float(m[3]) / 1000, "max_ms": float(m[4]) / 1000}
    m = grab(r"no fsync \(Off\)\s+(\d+) stmts/s\s+mean\s+([\d.]+) us\s+p50\s+([\d.]+)\s+p99\s+([\d.]+)\s+max\s+([\d.]+)")
    if m:
        out["commit (not durable)"] = {"per_s": float(m[0]), "mean_ms": float(m[1]) / 1000, "p50_ms": float(m[2]) / 1000,
                                       "p99_ms": float(m[3]) / 1000, "max_ms": float(m[4]) / 1000}
    m = grab(r"persistent, every row logged\+fsynced\s+([\d.]+) s\s+log ([\d.]+) MB")
    if m:
        out["bulk load (log)"] = {"s": float(m[0]), "log_mb": float(m[1])}
    m = grab(r"write checkpoint\s+([\d.]+) s\s+file ([\d.]+) MB")
    if m:
        out["checkpoint"] = {"s": float(m[0]), "bytes": float(m[1]) * 1024 * 1024}
    m = grab(r"open \+ load all tables\s+([\d.]+) s")
    if m:
        out["reopen"] = {"s": float(m[0])}
    m = grab(r"open \+ replay [\d.]+ MB of log\s+([\d.]+) s")
    if m:
        out["recovery by log replay"] = {"s": float(m[0])}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=float, default=1)
    ap.add_argument("--commits", type=int, default=2000)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--engines", default="cdb,sqlite,duckdb,chdb")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    result = {"sf": args.sf, "commits": args.commits, "threads": args.threads, "probe": common.control_probe(),
              "engines": {}}
    for e in args.engines.split(","):
        fn = {"sqlite": sqlite_run, "duckdb": duckdb_run, "chdb": chdb_run}.get(e)
        t = time.time()
        result["engines"][e] = cdb_run(args.sf, args.threads) if e == "cdb" else fn(args.sf, args.commits)
        print(e, f"{time.time() - t:.0f}s", flush=True)
    json.dump(result, open(args.out, "w"), indent=1)
    shutil.rmtree(SCRATCH, ignore_errors=True)


if __name__ == "__main__":
    main()
