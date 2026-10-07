#!/usr/bin/env python3
"""The Python-engine side of the comparison harness: one engine, one process, the same line
protocol as cdb_report_worker (see bench/report/cdb_worker.cpp).

    py_worker.py --engine duckdb|duckdb-decimal|datafusion|chdb|polars|sqlite

Requests are tab separated lines on stdin, replies one JSON object per line on stdout:

    THREADS n            start the engine with n threads
    LOADSPEC file        create and fill the tables of a JSON load specification (common.py)
    EXEC file            run the statements in the file (';' + newline separated)
    RUN file runs        run the query `runs` times; wall and CPU milliseconds per run
    DUMP file csv        run the query once and write its rows for the correctness check
    STAT                 peak / current resident set and thread count of this process
    QUIT

A query "file" is an .sql file, or for polars a .py module with query(tables) in it. The engine
materialises every result completely (an Arrow table / record batches where the API has them, row
tuples for SQLite) before the clock stops.
"""
import argparse
import importlib.util
import json
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

DECIMAL_COLUMNS = {"p_retailprice", "s_acctbal", "ps_supplycost", "c_acctbal", "o_totalprice",
                   "l_quantity", "l_extendedprice", "l_discount", "l_tax"}


def rows_to_text(rows):
    return [[common.cell_text(v) for v in r] for r in rows]


class Engine:
    name = "?"

    def open(self, threads):
        self.threads = threads

    def load(self, spec):
        raise NotImplementedError

    def exec_script(self, text):
        raise NotImplementedError

    def run(self, path):
        """Runs the query and materialises the whole result; returns the row count."""
        raise NotImplementedError

    def dump(self, path):
        """Runs the query and returns its rows as lists of Python values."""
        raise NotImplementedError

    @staticmethod
    def statements(text):
        return [s.strip() for s in text.split(";\n") if s.strip().rstrip(";")]


# ---------------------------------------------------------------------------------- DuckDB

class DuckDB(Engine):
    name = "duckdb"
    types = {"i32": "INTEGER", "i64": "BIGINT", "f64": "DOUBLE", "date": "DATE", "str": "VARCHAR"}
    decimal = False

    def open(self, threads):
        import duckdb
        super().open(threads)
        self.con = duckdb.connect(":memory:")
        self.con.execute(f"SET threads={threads}")

    def column_type(self, col, t):
        if self.decimal and col in DECIMAL_COLUMNS:
            return "DECIMAL(15,2)"
        return self.types[t]

    def load(self, spec):
        out = {}
        for t in spec["tables"]:
            cols = ", ".join(f"{c} {self.column_type(c, ty)} NOT NULL" for c, ty in t["columns"])
            start = time.perf_counter()
            self.con.execute(f"CREATE TABLE {t['name']} ({cols})")
            header = "true" if t["header"] else "false"
            self.con.execute(f"COPY {t['name']} FROM '{t['path']}' "
                             f"(DELIMITER '{t['delimiter']}', HEADER {header})")
            rows = self.con.execute(f"SELECT count(*) FROM {t['name']}").fetchone()[0]
            out[t["name"]] = {"ms": (time.perf_counter() - start) * 1000, "rows": rows}
        return out

    def exec_script(self, text):
        for s in self.statements(text):
            self.con.execute(s)

    def run(self, path):
        return self.con.execute(open(path).read()).fetch_arrow_table().num_rows

    def dump(self, path):
        return self.con.execute(open(path).read()).fetchall()


class DuckDBDecimal(DuckDB):
    name = "duckdb-decimal"
    decimal = True


# ---------------------------------------------------------------------------------- DataFusion

class DataFusion(Engine):
    name = "datafusion"

    def open(self, threads):
        import datafusion
        super().open(threads)
        cfg = datafusion.SessionConfig().with_target_partitions(threads)
        self.ctx = datafusion.SessionContext(cfg)

    def load(self, spec):
        import pyarrow as pa
        types = {"i32": pa.int32(), "i64": pa.int64(), "f64": pa.float64(), "date": pa.date32(),
                 "str": pa.string()}
        out = {}
        for t in spec["tables"]:
            schema = pa.schema([pa.field(c, types[ty], nullable=False) for c, ty in t["columns"]])
            start = time.perf_counter()
            batches = self.ctx.read_csv(t["path"], schema=schema, has_header=t["header"],
                                        delimiter=t["delimiter"]).collect()
            # an in-memory table with one partition per thread, as DataFusion's own benchmarks
            # arrange memory tables (a single partition would leave the scan on one task)
            parts = [[] for _ in range(self.threads)]
            for i, b in enumerate(batches):
                parts[i * self.threads // max(1, len(batches))].append(b)
            self.ctx.register_record_batches(t["name"], [p for p in parts if p] or [[]])
            out[t["name"]] = {"ms": (time.perf_counter() - start) * 1000,
                              "rows": sum(b.num_rows for b in batches)}
        return out

    def exec_script(self, text):
        for s in self.statements(text):
            self.ctx.sql(s).collect()

    def run(self, path):
        return sum(b.num_rows for b in self.ctx.sql(open(path).read()).collect())

    def dump(self, path):
        rows = []
        for b in self.ctx.sql(open(path).read()).collect():
            cols = [b.column(i).to_pylist() for i in range(b.num_columns)]
            rows.extend(zip(*cols))
        return rows


# ---------------------------------------------------------------------------------- ClickHouse

class ChDB(Engine):
    name = "chdb"
    types = {"i32": "Int32", "i64": "Int64", "f64": "Float64", "date": "Date", "str": "String"}
    engine = "Memory"

    def open(self, threads):
        from chdb import session
        super().open(threads)
        self.sess = session.Session()
        self.sess.query(f"SET max_threads = {threads}")
        # SQL-standard semantics that ClickHouse does not have by default and that TPC-H needs:
        # a LEFT JOIN pads with NULL (not with the type's default, Q13), and an aggregate over no
        # rows is NULL (not 0, Q17 at small scale factors). Listed in the report's appendix.
        self.sess.query("SET join_use_nulls = 1")
        self.sess.query("SET aggregate_functions_null_for_empty = 1")

    def load(self, spec):
        out = {}
        for t in spec["tables"]:
            cols = ", ".join(f"{c} {self.types[ty]}" for c, ty in t["columns"])
            schema = ", ".join(f"{c} {self.types[ty]}" for c, ty in t["columns"])
            order = " ORDER BY tuple()" if self.engine != "Memory" else ""
            start = time.perf_counter()
            self.sess.query(f"CREATE TABLE {t['name']} ({cols}) ENGINE = {self.engine}{order}")
            self.sess.query(f"INSERT INTO {t['name']} SELECT * FROM "
                            f"file('{t['path']}', 'CSV', '{schema}') "
                            f"SETTINGS format_csv_delimiter = '{t['delimiter']}', "
                            f"input_format_csv_trim_whitespaces = 0, "
                            f"max_threads = {self.threads}")
            rows = int(self.sess.query(f"SELECT count() FROM {t['name']}", "CSV").bytes().strip())
            out[t["name"]] = {"ms": (time.perf_counter() - start) * 1000, "rows": rows}
        return out

    def exec_script(self, text):
        for s in self.statements(text):
            self.sess.query(s)

    def _query(self, path, fmt):
        return self.sess.query(open(path).read().strip().rstrip(";"), fmt)

    def run(self, path):
        import pyarrow as pa
        buf = self._query(path, "Arrow").bytes()  # the Arrow IPC file format
        return pa.ipc.open_file(pa.BufferReader(buf)).read_all().num_rows if buf else 0

    def dump(self, path):
        import pyarrow as pa
        buf = self._query(path, "Arrow").bytes()
        table = pa.ipc.open_file(pa.BufferReader(buf)).read_all()
        cols = [table.column(i).to_pylist() for i in range(table.num_columns)]
        return list(zip(*cols))


class ChDBMergeTree(ChDB):
    name = "chdb-mergetree"
    engine = "MergeTree"


# ---------------------------------------------------------------------------------- Polars

class Polars(Engine):
    name = "polars"

    def open(self, threads):
        import polars as pl
        super().open(threads)
        self.pl = pl
        sys.path.insert(0, os.path.join(common.REPORT_DIR, "queries", "polars"))
        self.tables = {}
        self.modules = {}

    def load(self, spec):
        pl = self.pl
        types = {"i32": pl.Int32, "i64": pl.Int64, "f64": pl.Float64, "date": pl.Date,
                 "str": pl.String}
        out = {}
        for t in spec["tables"]:
            names = [c for c, _ in t["columns"]]
            start = time.perf_counter()
            df = pl.read_csv(t["path"], separator=t["delimiter"], has_header=t["header"],
                             new_columns=names, schema_overrides={c: types[ty] for c, ty in t["columns"]},
                             try_parse_dates=False)
            self.tables[t["name"]] = df
            out[t["name"]] = {"ms": (time.perf_counter() - start) * 1000, "rows": df.height}
        return out

    def _module(self, path):
        if path not in self.modules:
            spec = importlib.util.spec_from_file_location(f"pq_{len(self.modules)}", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            self.modules[path] = mod
        return self.modules[path]

    def _collect(self, path):
        if path.endswith(".sql"):  # the micro-benchmark and H2O queries: Polars' SQL interface
            if getattr(self, "sql_ctx", None) is None:
                self.sql_ctx = self.pl.SQLContext({n: df.lazy() for n, df in self.tables.items()})
            return self.sql_ctx.execute(open(path).read().strip().rstrip(";")).collect()
        res = self._module(path).query(self.tables)
        return res.collect() if hasattr(res, "collect") else res

    def run(self, path):
        return self._collect(path).height

    def dump(self, path):
        return self._collect(path).rows()

    def exec_script(self, text):
        raise NotImplementedError("polars has no SQL scripts")


# ---------------------------------------------------------------------------------- SQLite

SQLITE_INDEXES = [
    "CREATE UNIQUE INDEX i_nation ON nation(n_nationkey)",
    "CREATE INDEX i_nation_r ON nation(n_regionkey)",
    "CREATE UNIQUE INDEX i_region ON region(r_regionkey)",
    "CREATE UNIQUE INDEX i_part ON part(p_partkey)",
    "CREATE UNIQUE INDEX i_supplier ON supplier(s_suppkey)",
    "CREATE INDEX i_supplier_n ON supplier(s_nationkey)",
    "CREATE UNIQUE INDEX i_partsupp ON partsupp(ps_partkey, ps_suppkey)",
    "CREATE INDEX i_partsupp_s ON partsupp(ps_suppkey)",
    "CREATE UNIQUE INDEX i_customer ON customer(c_custkey)",
    "CREATE INDEX i_customer_n ON customer(c_nationkey)",
    "CREATE UNIQUE INDEX i_orders ON orders(o_orderkey)",
    "CREATE INDEX i_orders_c ON orders(o_custkey)",
    "CREATE INDEX i_lineitem ON lineitem(l_orderkey, l_linenumber)",
    "CREATE INDEX i_lineitem_p ON lineitem(l_partkey, l_suppkey)",
    "CREATE INDEX i_lineitem_s ON lineitem(l_suppkey)",
]


class SQLite(Engine):
    name = "sqlite"
    types = {"i32": "INTEGER", "i64": "INTEGER", "f64": "REAL", "date": "TEXT", "str": "TEXT"}

    def open(self, threads):
        import sqlite3
        super().open(threads)
        self.con = sqlite3.connect(":memory:")
        self.indexes = os.environ.get("SQLITE_INDEXES", "1") == "1"

    def load(self, spec):
        import csv
        out = {}
        for t in spec["tables"]:
            cols = ", ".join(f"{c} {self.types[ty]} NOT NULL" for c, ty in t["columns"])
            start = time.perf_counter()
            self.con.execute(f"CREATE TABLE {t['name']} ({cols})")
            conv = [int if ty in ("i32", "i64") else float if ty == "f64" else None
                    for _, ty in t["columns"]]
            marks = ",".join("?" * len(conv))
            with open(t["path"], newline="") as f:
                reader = csv.reader(f, delimiter=t["delimiter"])
                if t["header"]:
                    next(reader)

                def rows():
                    for r in reader:
                        yield [c(v) if c else v for c, v in zip(conv, r)]
                self.con.executemany(f"INSERT INTO {t['name']} VALUES ({marks})", rows())
            self.con.commit()
            n = self.con.execute(f"SELECT count(*) FROM {t['name']}").fetchone()[0]
            out[t["name"]] = {"ms": (time.perf_counter() - start) * 1000, "rows": n}
        if self.indexes and any(t["name"] == "lineitem" for t in spec["tables"]):
            start = time.perf_counter()
            for s in SQLITE_INDEXES:
                self.con.execute(s)
            self.con.execute("ANALYZE")
            out["_indexes"] = {"ms": (time.perf_counter() - start) * 1000, "rows": 0}
        return out

    def exec_script(self, text):
        for s in self.statements(text):
            self.con.execute(s)

    def run(self, path):
        return len(self.con.execute(open(path).read().strip().rstrip(";")).fetchall())

    def dump(self, path):
        return self.con.execute(open(path).read().strip().rstrip(";")).fetchall()


ENGINES = {c.name: c for c in (DuckDB, DuckDBDecimal, DataFusion, ChDB, ChDBMergeTree, Polars, SQLite)}


# ---------------------------------------------------------------------------------- protocol

def status_kb(key):
    for line in open("/proc/self/status"):
        if line.startswith(key):
            return int(line.split()[1])
    return -1


def reply(**kw):
    sys.stdout.write(json.dumps(kw) + "\n")
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", required=True, choices=sorted(ENGINES))
    args = ap.parse_args()
    engine = ENGINES[args.engine]()
    for line in sys.stdin:
        f = line.rstrip("\n").split("\t")
        op = f[0]
        try:
            if op == "QUIT":
                reply(ok=True)
                return
            if op == "THREADS":
                engine.open(int(f[1]))
                reply(ok=True, threads=int(f[1]))
            elif op == "STAT":
                reply(ok=True, vm_hwm_kb=status_kb("VmHWM:"), vm_rss_kb=status_kb("VmRSS:"),
                      os_threads=status_kb("Threads:"))
            elif op == "LOADSPEC":
                reply(ok=True, tables=engine.load(json.load(open(f[1]))))
            elif op == "EXEC":
                t = time.perf_counter()
                cpu = time.process_time()
                engine.exec_script(open(f[1]).read())
                reply(ok=True, ms=(time.perf_counter() - t) * 1000,
                      cpu_ms=(time.process_time() - cpu) * 1000)
            elif op == "RUN":
                wall, cpu, rows = [], [], 0
                for _ in range(int(f[2])):
                    t, c = time.perf_counter(), time.process_time()
                    rows = engine.run(f[1])
                    wall.append((time.perf_counter() - t) * 1000)
                    cpu.append((time.process_time() - c) * 1000)
                reply(ok=True, rows=rows, wall_ms=wall, cpu_ms=cpu)
            elif op == "DUMP":
                rows = engine.dump(f[1])
                common.write_pipe_csv(f[2], rows_to_text(rows))
                reply(ok=True, rows=len(rows))
            else:
                reply(ok=False, error=f"unknown request {op}")
        except Exception as e:  # reported to the driver, which decides what a failure means
            reply(ok=False, error=f"{type(e).__name__}: {e}", trace=traceback.format_exc()[-1500:])


if __name__ == "__main__":
    main()
