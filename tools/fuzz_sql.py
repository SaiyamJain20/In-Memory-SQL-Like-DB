#!/usr/bin/env python3
"""Random SQL differential testing against DuckDB.

    .venv/bin/python tools/fuzz_sql.py --seeds 1-4 --queries 300     # writes data/sqlfuzz/seed<N>.test
    CDB_REQUIRE_SQLFUZZ=1 build/debug/tests/cdb_tests --gtest_filter='SqlFuzz*'

For each seed it builds a few small random tables (NULL-heavy, small value domains so that joins and
predicates match), then generates random queries - filters, expressions, joins of every kind,
aggregation, DISTINCT, ORDER BY / LIMIT, derived tables, CTEs, and subqueries (IN / NOT IN / EXISTS /
NOT EXISTS / scalar, correlated and not) - keeps those DuckDB accepts, and writes them with DuckDB's
answers in the sqllogictest-style format of tests/sql/*.test (see tools/gen_slt.py). The C++ runner
(tests/sql/slt_runner_test.cpp) then runs the files against this engine, with and without the
optimizer. A query that needs a feature this engine does not have yet (it answers NotImplemented) is
counted, not failed; any other difference is a failure and prints the query.

Generated queries stay inside what both engines agree on (docs/adr/0003): exact doubles (multiples
of 1/4, only + - *), no division by a column, ASCII text, no implicit casts between text and numbers.
They also avoid the shapes this engine documents as unsupported (correlated NOT IN, IN / EXISTS under
OR, ...), so that "NotImplemented" stays rare and a skip rate above a few percent is itself a finding.
"""
import argparse
import datetime
import os
import random
import sys
import time

import duckdb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_slt  # noqa: E402  (shares the answer rendering and the file format)

STRINGS = ["a", "ab", "abc", "b", "ba", "bb", "", "xyz", "Ab"]
BASE_DATE = datetime.date(2000, 1, 1)


class Schema:
    def __init__(self, rng, big=False):
        self.rng = rng
        self.big = big
        self.tables = {
            "t1": [("a", "INTEGER"), ("b", "INTEGER"), ("c", "VARCHAR"), ("d", "DOUBLE"), ("e", "DATE")],
            "t2": [("a", "INTEGER"), ("x", "INTEGER"), ("y", "VARCHAR"), ("z", "DOUBLE")],
            "t3": [("k", "INTEGER"), ("a", "INTEGER"), ("w", "VARCHAR")],
        }
        self.rows = {}
        for name, cols in self.tables.items():
            n = rng.choice([0, 1, 5, 12, 25, 40, 60]) if name != "t1" else rng.choice([8, 20, 40, 60])
            if big:  # more than a vector (2048 rows) and several small row groups
                n = {"t1": 3000, "t2": 2200, "t3": 100}[name]
            null_rate = rng.choice([0.0, 0.1, 0.25])
            self.rows[name] = [[self.value(t, null_rate) for _, t in cols] for _ in range(n)]

    def value(self, typ, null_rate):
        rng = self.rng
        if rng.random() < null_rate:
            return None
        if typ == "INTEGER":
            return rng.randint(0, 9)
        if typ == "VARCHAR":
            return rng.choice(STRINGS)
        if typ == "DOUBLE":
            return rng.randint(-32, 32) / 4.0
        if typ == "DATE":
            return BASE_DATE + datetime.timedelta(days=rng.randint(0, 400))
        raise AssertionError(typ)

    def statements(self):
        out = []
        for name, cols in self.tables.items():
            out.append(f"CREATE TABLE {name} (" + ", ".join(f"{c} {t}" for c, t in cols) + ");")
            step = 200 if self.big else 20
            for i in range(0, len(self.rows[name]), step):
                chunk = self.rows[name][i:i + step]
                out.append(f"INSERT INTO {name} VALUES " + ", ".join(
                    "(" + ", ".join(literal(v) for v in row) + ")" for row in chunk) + ";")
        return out


def literal(v):
    if v is None:
        return "NULL"
    if isinstance(v, str):
        return "'" + v.replace("'", "''") + "'"
    if isinstance(v, datetime.date):
        return f"DATE '{v.isoformat()}'"
    return repr(v)


class Scope:
    """The columns a query block can reference: [(qualified name, type)]."""

    def __init__(self, columns):
        self.columns = columns

    def of(self, typ):
        return [n for n, t in self.columns if t == typ]


class Gen:
    def __init__(self, rng, schema, con):
        self.rng = rng
        self.schema = schema
        self.con = con  # DuckDB, with the tables, to ask what type an expression has
        self.alias_counter = 0

    # ---------------------------------------------------------------- expressions
    def int_const(self):
        return str(self.rng.randint(0, 9))

    def int_expr(self, scope, depth=0):
        rng = self.rng
        cols = scope.of("INTEGER")
        choice = rng.random()
        if depth >= 2 or choice < 0.45:
            if cols and rng.random() < 0.8:
                return rng.choice(cols)
            return self.int_const()
        if choice < 0.6:
            return f"({self.int_expr(scope, depth + 1)} + {self.int_expr(scope, depth + 1)})"
        if choice < 0.7:
            return f"({self.int_expr(scope, depth + 1)} - {self.int_expr(scope, depth + 1)})"
        if choice < 0.78:
            return f"({self.int_expr(scope, depth + 1)} * {rng.randint(0, 3)})"
        if choice < 0.86:
            return f"coalesce({self.int_expr(scope, depth + 1)}, {self.int_const()})"
        if choice < 0.93:
            return (f"CASE WHEN {self.bool_expr(scope, depth + 1)} THEN {self.int_expr(scope, depth + 1)} "
                    f"ELSE {self.int_expr(scope, depth + 1)} END")
        strs = scope.of("VARCHAR")
        if strs:
            return f"length({rng.choice(strs)})"
        return self.int_const()

    def dbl_expr(self, scope, depth=0):
        rng = self.rng
        cols = scope.of("DOUBLE")
        if depth >= 2 or rng.random() < 0.5:
            if cols and rng.random() < 0.85:
                return rng.choice(cols)
            return f"{rng.randint(-8, 8) / 4.0!r}e0"  # (1.25 alone would be a DECIMAL in DuckDB)
        if rng.random() < 0.5:
            return f"({self.dbl_expr(scope, depth + 1)} + {self.dbl_expr(scope, depth + 1)})"
        if rng.random() < 0.5:
            return f"({self.dbl_expr(scope, depth + 1)} - {self.dbl_expr(scope, depth + 1)})"
        return f"({self.dbl_expr(scope, depth + 1)} * {rng.randint(1, 3)})"

    def str_expr(self, scope, depth=0):
        rng = self.rng
        cols = scope.of("VARCHAR")
        if depth >= 2 or rng.random() < 0.55:
            if cols and rng.random() < 0.85:
                return rng.choice(cols)
            return literal(rng.choice(STRINGS))
        r = rng.random()
        if r < 0.3:
            return f"upper({self.str_expr(scope, depth + 1)})"
        if r < 0.55:
            return f"lower({self.str_expr(scope, depth + 1)})"
        if r < 0.8:
            return f"({self.str_expr(scope, depth + 1)} || {self.str_expr(scope, depth + 1)})"
        return f"substring({self.str_expr(scope, depth + 1)} FROM {rng.randint(1, 2)} FOR {rng.randint(1, 3)})"

    def date_expr(self, scope, interval_ok=True):
        cols = scope.of("DATE")
        if interval_ok and self.rng.random() < 0.12:
            # (DuckDB types DATE + INTERVAL as a TIMESTAMP and this engine as a DATE: only compared)
            # (only a constant date takes an INTERVAL in this engine; a column is a known gap)
            unit = self.rng.choice(["DAY", "DAY", "MONTH"])
            sign = self.rng.choice(["+", "-"])
            base = BASE_DATE + datetime.timedelta(days=self.rng.randint(0, 400))
            return f"(DATE '{base.isoformat()}' {sign} INTERVAL '{self.rng.randint(1, 40)}' {unit})"
        if cols and self.rng.random() < 0.8:
            return self.rng.choice(cols)
        d = BASE_DATE + datetime.timedelta(days=self.rng.randint(0, 400))
        return f"DATE '{d.isoformat()}'"

    def bool_expr(self, scope, depth=0):
        rng = self.rng
        if depth >= 2:
            return self.atom(scope, depth)
        r = rng.random()
        if r < 0.6:
            return self.atom(scope, depth)
        if r < 0.78:
            return f"({self.bool_expr(scope, depth + 1)} AND {self.bool_expr(scope, depth + 1)})"
        if r < 0.92:
            return f"({self.bool_expr(scope, depth + 1)} OR {self.bool_expr(scope, depth + 1)})"
        return f"NOT ({self.bool_expr(scope, depth + 1)})"

    def atom(self, scope, depth):
        rng = self.rng
        ops = ["=", "<>", "<", "<=", ">", ">="]
        kinds = ["int", "int", "int", "str", "dbl", "date", "null", "in", "between", "like"]
        for _ in range(8):
            k = rng.choice(kinds)
            if k == "int" and scope.of("INTEGER"):
                return f"{self.int_expr(scope, depth + 1)} {rng.choice(ops)} {self.int_expr(scope, depth + 1)}"
            if k == "str" and scope.of("VARCHAR"):
                return f"{self.str_expr(scope, depth + 1)} {rng.choice(ops)} {self.str_expr(scope, depth + 1)}"
            if k == "dbl" and scope.of("DOUBLE"):
                return f"{self.dbl_expr(scope, depth + 1)} {rng.choice(ops)} {self.dbl_expr(scope, depth + 1)}"
            if k == "date" and scope.of("DATE"):
                return f"{self.date_expr(scope)} {rng.choice(ops)} {self.date_expr(scope)}"
            if k == "null" and scope.columns:
                col = rng.choice(scope.columns)[0]
                return f"{col} IS {'NOT ' if rng.random() < 0.5 else ''}NULL"
            if k == "in" and scope.of("INTEGER"):
                items = ", ".join(self.int_const() for _ in range(rng.randint(1, 4)))
                return f"{self.int_expr(scope, depth + 1)} {'NOT ' if rng.random() < 0.3 else ''}IN ({items})"
            if k == "between" and scope.of("INTEGER"):
                lo = rng.randint(0, 6)
                return f"{self.int_expr(scope, depth + 1)} BETWEEN {lo} AND {lo + rng.randint(0, 4)}"
            if k == "like" and scope.of("VARCHAR"):
                pattern = rng.choice(["a%", "%b", "%a%", "ab", "_b%", "%"])
                return f"{rng.choice(scope.of('VARCHAR'))} {'NOT ' if rng.random() < 0.2 else ''}LIKE '{pattern}'"
        return "1 = 1"

    def select_item(self, scope):
        rng = self.rng
        kinds = [t for t in ("INTEGER", "VARCHAR", "DOUBLE", "DATE") if scope.of(t)]
        kind = rng.choice(kinds) if kinds else None
        if kind is None:
            return "1"
        r = rng.random()
        if r < 0.5:
            return rng.choice(scope.of(kind))
        if kind == "INTEGER":
            return self.int_expr(scope)
        if kind == "VARCHAR":
            return self.str_expr(scope)
        if kind == "DOUBLE":
            return self.dbl_expr(scope)
        return self.date_expr(scope, interval_ok=False)

    # ---------------------------------------------------------------- relations
    def table_scope(self, name, alias):
        return Scope([(f"{alias}.{c}", t) for c, t in self.schema.tables[name]])

    def new_alias(self):
        self.alias_counter += 1
        return f"q{self.alias_counter}"

    def from_clause(self):
        """Returns (sql, scope)."""
        rng = self.rng
        names = list(self.schema.tables)
        kind = rng.choice(["one", "one", "join", "join", "join3", "cross", "derived", "self"])
        if self.schema.big and kind in ("cross", "join3", "self"):
            kind = "join"  # (joins of big tables on low-cardinality keys explode: one big x t3 only)
        if kind == "one":
            n = rng.choice(names)
            a = self.new_alias()
            return f"{n} AS {a}", self.table_scope(n, a)
        if kind == "self":
            n = rng.choice(names)
            a, b = self.new_alias(), self.new_alias()
            sa, sb = self.table_scope(n, a), self.table_scope(n, b)
            col = rng.choice([c for c, t in self.schema.tables[n] if t == "INTEGER"])
            return (f"{n} AS {a} JOIN {n} AS {b} ON {a}.{col} = {b}.{col}",
                    Scope(sa.columns + sb.columns))
        if kind == "derived" and rng.random() < 0.3:
            n = rng.choice(names)
            a = self.new_alias()
            inner = self.table_scope(n, "s")
            ints = inner.of("INTEGER")
            key = rng.choice(ints)
            agg = rng.choice(["count(*)", f"sum({rng.choice(ints)})", f"min({rng.choice(ints)})", f"max({rng.choice(ints)})"])
            sql = f"(SELECT {key} AS g, {agg} AS m, count(*) AS n FROM {n} AS s GROUP BY {key}) AS {a}"
            cols = [(f"{a}.g", "INTEGER"), (f"{a}.m", "INTEGER"), (f"{a}.n", "INTEGER")]
            if rng.random() < 0.5:  # joined to a base table on the group key
                n2 = rng.choice(names)
                b = self.new_alias()
                sb = self.table_scope(n2, b)
                sql += f" JOIN {n2} AS {b} ON {a}.g = {rng.choice(sb.of('INTEGER'))}"
                cols += sb.columns
            return sql, Scope(cols)
        if kind == "derived":
            n = rng.choice(names)
            a = self.new_alias()
            inner = self.table_scope(n, "s")
            items, cols = [], []
            for i in range(rng.randint(2, 3)):
                expr = self.select_item(inner)
                t = self.type_of(expr, n)
                items.append(f"{expr} AS v{i}")
                cols.append((f"{a}.v{i}", t))
            where = f" WHERE {self.bool_expr(inner)}" if rng.random() < 0.5 else ""
            return f"(SELECT {', '.join(items)} FROM {n} AS s{where}) AS {a}", Scope(cols)
        if kind == "cross":
            n1, n2 = rng.sample(names, 2)
            a, b = self.new_alias(), self.new_alias()
            sa, sb = self.table_scope(n1, a), self.table_scope(n2, b)
            return f"{n1} AS {a}, {n2} AS {b}", Scope(sa.columns + sb.columns)
        # joins
        count = 3 if kind == "join3" else 2
        picked = [rng.choice(names) for _ in range(count)]
        if self.schema.big:
            picked = rng.sample([rng.choice(["t1", "t2"]), "t3"], 2)
        aliases = [self.new_alias() for _ in picked]
        scopes = [self.table_scope(n, a) for n, a in zip(picked, aliases)]
        sql = f"{picked[0]} AS {aliases[0]}"
        columns = list(scopes[0].columns)
        for i in range(1, count):
            jt = rng.choice(["JOIN", "JOIN", "LEFT JOIN", "RIGHT JOIN"])
            left_int = [c for c, t in columns if t == "INTEGER"]
            right_int = scopes[i].of("INTEGER")
            on = f"{rng.choice(left_int)} = {rng.choice(right_int)}"
            if rng.random() < 0.3:
                on += f" AND {self.bool_expr(Scope(columns + scopes[i].columns), 1)}"
            sql += f" {jt} {picked[i]} AS {aliases[i]} ON {on}"
            columns += scopes[i].columns
        return sql, Scope(columns)

    def type_of(self, expr, table):
        """The type DuckDB gives `expr` evaluated over `table` aliased `s`, as one of our four."""
        desc = self.con.execute(f"SELECT {expr} FROM {table} AS s LIMIT 0").description
        code = str(desc[0][1]).upper()
        if "INT" in code:
            return "INTEGER"
        if "DOUBLE" in code or "FLOAT" in code:
            return "DOUBLE"
        if "DATE" in code:
            return "DATE"
        return "VARCHAR"

    # ---------------------------------------------------------------- subqueries
    def subquery_conjunct(self, scope):
        """A WHERE conjunct with a subquery over `scope`'s columns (never under OR / NOT)."""
        rng = self.rng
        names = list(self.schema.tables)
        n = rng.choice(names)
        s = self.new_alias()
        inner = self.table_scope(n, s)
        outer_int = scope.of("INTEGER")
        inner_int = inner.of("INTEGER")
        if not outer_int or not inner_int:
            return None
        kind = rng.choice(["in", "not_in", "exists", "not_exists", "scalar", "scalar_corr", "exists_resid",
                           "in_corr", "nested"])
        inner_where = f" WHERE {self.bool_expr(inner, 1)}" if rng.random() < 0.5 else ""
        if kind == "in":
            return f"{rng.choice(outer_int)} IN (SELECT {rng.choice(inner_int)} FROM {n} AS {s}{inner_where})"
        if kind == "not_in":
            return f"{rng.choice(outer_int)} NOT IN (SELECT {rng.choice(inner_int)} FROM {n} AS {s}{inner_where})"
        corr = f"{rng.choice(inner_int)} = {rng.choice(outer_int)}"
        if kind == "exists":
            extra = f" AND {self.bool_expr(inner, 1)}" if rng.random() < 0.4 else ""
            return f"EXISTS (SELECT 1 FROM {n} AS {s} WHERE {corr}{extra})"
        if kind == "not_exists":
            extra = f" AND {self.bool_expr(inner, 1)}" if rng.random() < 0.4 else ""
            return f"NOT EXISTS (SELECT 1 FROM {n} AS {s} WHERE {corr}{extra})"
        if kind == "exists_resid":
            op = rng.choice(["<", "<=", ">", ">=", "<>"])
            resid = f"{rng.choice(inner_int)} {op} {rng.choice(outer_int)}"
            neg = "NOT " if rng.random() < 0.5 else ""
            return f"{neg}EXISTS (SELECT 1 FROM {n} AS {s} WHERE {corr} AND {resid})"
        if kind == "in_corr":
            return (f"{rng.choice(outer_int)} IN (SELECT {rng.choice(inner_int)} FROM {n} AS {s} "
                    f"WHERE {rng.choice(inner_int)} = {rng.choice(outer_int)})")
        if kind == "nested":
            n2 = rng.choice(names)
            s2 = self.new_alias()
            inner2 = self.table_scope(n2, s2)
            return (f"{rng.choice(outer_int)} IN (SELECT {rng.choice(inner_int)} FROM {n} AS {s} WHERE "
                    f"EXISTS (SELECT 1 FROM {n2} AS {s2} WHERE {rng.choice(inner2.of('INTEGER'))} = "
                    f"{rng.choice(inner_int)}))")
        agg = rng.choice(["min", "max", "sum", "count", "avg"])
        arg = rng.choice(inner_int)
        agg_sql = "count(*)" if agg == "count" and rng.random() < 0.5 else f"{agg}({arg})"
        op = rng.choice(["=", "<", "<=", ">", ">=", "<>"])
        if kind == "scalar":
            return f"{self.int_expr(scope, 1)} {op} (SELECT {agg_sql} FROM {n} AS {s}{inner_where})"
        return (f"{self.int_expr(scope, 1)} {op} (SELECT {agg_sql} FROM {n} AS {s} WHERE "
                f"{rng.choice(inner_int)} = {rng.choice(outer_int)})")

    # ---------------------------------------------------------------- statements that change data
    def mutation(self):
        """INSERT ... VALUES / INSERT ... SELECT into one of the tables."""
        rng = self.rng
        name = rng.choice(list(self.schema.tables))
        cols = self.schema.tables[name]
        if rng.random() < 0.5:
            rows = [[self.schema.value(t, 0.2) for _, t in cols] for _ in range(rng.randint(1, 4))]
            return f"INSERT INTO {name} VALUES " + ", ".join(
                "(" + ", ".join(literal(v) for v in row) + ")" for row in rows)
        src = rng.choice(list(self.schema.tables))
        s = self.new_alias()
        scope = self.table_scope(src, s)
        items = []
        for _, t in cols:
            candidates = scope.of(t)
            items.append(rng.choice(candidates) if candidates else literal(self.schema.value(t, 0.0)))
        where = f" WHERE {self.bool_expr(scope, 1)}" if rng.random() < 0.6 else ""
        return f"INSERT INTO {name} SELECT {', '.join(items)} FROM {src} AS {s}{where}"

    # ---------------------------------------------------------------- queries
    def query(self):
        rng = self.rng
        from_sql, scope = self.from_clause()
        conjuncts = []
        for _ in range(rng.choice([0, 1, 1, 2])):
            conjuncts.append(self.bool_expr(scope))
        subs = 0
        if rng.random() < 0.45:
            for _ in range(rng.choice([1, 1, 2])):
                c = self.subquery_conjunct(scope)
                if c:
                    conjuncts.append(c)
                    subs += 1
        where = " WHERE " + " AND ".join(conjuncts) if conjuncts else ""
        shape = rng.choice(["plain", "plain", "plain", "agg", "agg", "distinct", "scalar_item"])
        if self.schema.big:  # keep the answers small: aggregates over at most one group column
            shape = "agg"
        tail = ""
        if shape == "agg":
            groups = rng.sample([c for c, _ in scope.columns],
                                k=min(len(scope.columns), rng.choice([0, 1, 1, 1] if self.schema.big else [0, 1, 1, 2])))
            if self.schema.big:  # grouping by a key-like column would return thousands of rows
                groups = [g for g in groups if g.split(".")[1] in ("a", "b", "k", "x", "c", "y", "w")][:1]
            if groups and scope.of("INTEGER") and rng.random() < 0.2:  # GROUP BY an expression
                groups = [f"({rng.choice(scope.of('INTEGER'))} + {rng.randint(0, 3)})"]
            aggs = []
            for _ in range(rng.randint(1, 3)):
                f = rng.choice(["count(*)", "count", "sum", "min", "max", "avg", "count_distinct"])
                col = rng.choice(scope.columns)
                if f == "count(*)":
                    aggs.append("count(*)")
                elif f == "count":
                    aggs.append(f"count({col[0]})")
                elif f == "count_distinct":
                    aggs.append(f"count(DISTINCT {col[0]})")
                elif f in ("sum", "avg"):
                    nums = scope.of("INTEGER") + scope.of("DOUBLE")
                    if nums and rng.random() < 0.3 and scope.of("INTEGER"):
                        aggs.append(f"{f}({self.int_expr(scope, 1)})")
                    else:
                        aggs.append(f"{f}({rng.choice(nums)})" if nums else "count(*)")
                else:
                    aggs.append(f"{f}({col[0]})")
            items = groups + aggs
            if groups:
                tail = " GROUP BY " + ", ".join(groups)
                if rng.random() < 0.3:
                    tail += f" HAVING count(*) {rng.choice(['>', '>=', '<'])} {rng.randint(0, 3)}"
                elif rng.random() < 0.15:
                    n = rng.choice(list(self.schema.tables))
                    tail += f" HAVING count(*) {rng.choice(['>', '<='])} (SELECT count(*) FROM {n}) / 8"
            select = ", ".join(items)
            distinct = ""
            ncols = len(items)
        elif shape == "scalar_item" and not any("EXISTS" in c or " IN (" in c for c in conjuncts):
            items = [self.select_item(scope) for _ in range(rng.randint(1, 2))]
            inner_names = list(self.schema.tables)
            n = rng.choice(inner_names)
            s = self.new_alias()
            inner = self.table_scope(n, s)
            agg = rng.choice(["max", "min", "count(*)", "sum"])
            inner_int = inner.of("INTEGER")
            arg = rng.choice(inner_int)
            expr = "count(*)" if agg == "count(*)" else f"{agg}({arg})"
            if scope.of("INTEGER") and rng.random() < 0.5:
                items.append(f"(SELECT {expr} FROM {n} AS {s} WHERE {rng.choice(inner_int)} = "
                             f"{rng.choice(scope.of('INTEGER'))})")
            else:
                items.append(f"(SELECT {expr} FROM {n} AS {s})")
            select = ", ".join(items)
            distinct = ""
            ncols = len(items)
        else:
            items = [self.select_item(scope) for _ in range(rng.randint(1, 4))]
            select = ", ".join(items)
            distinct = "DISTINCT " if shape == "distinct" else ""
            ncols = len(items)
        sql = f"SELECT {distinct}{select} FROM {from_sql}{where}{tail}"
        ordered = False
        if rng.random() < 0.35:
            ordered = True
            lead = ""
            if shape == "plain" and not distinct and scope.of("INTEGER") and rng.random() < 0.3:
                # an ordering by an expression that is not in the select list, then every output column
                lead = f"{self.int_expr(scope, 1)}{' DESC' if rng.random() < 0.5 else ''}, "
            sql += " ORDER BY " + lead + ", ".join(
                f"{i}{' DESC' if rng.random() < 0.3 else ''}" for i in range(1, ncols + 1))
            if rng.random() < 0.5:
                sql += f" LIMIT {rng.randint(0, 12)}"
                if rng.random() < 0.3:
                    sql += f" OFFSET {rng.randint(0, 5)}"
        # sometimes through a CTE
        if rng.random() < 0.12:
            sql = f"WITH w AS ({sql.split(' ORDER BY ')[0]}) SELECT * FROM w"
            ordered = False
        return sql, ordered


def letters(description):
    out = ""
    for _name, code, *_ in description:
        t = str(code).upper()
        if "INT" in t:
            out += "I"
        elif "DOUBLE" in t or "FLOAT" in t or "DECIMAL" in t:
            out += "R"
        else:
            out += "T"
    return out


def generate(seed, count, out_path):
    rng = random.Random(seed)
    big = seed % 5 == 0
    schema = Schema(rng, big)
    con = duckdb.connect()
    gen = Gen(rng, schema, con)
    blocks = []
    for stmt in schema.statements():
        con.execute(stmt)
        blocks.append("statement ok\n" + stmt)
    accepted = rejected = 0
    attempts = 0
    while accepted < count and attempts < count * 10:
        attempts += 1
        if rng.random() < 0.04 and not big and all(len(r) < 150 for r in schema.rows.values()):
            stmt = gen.mutation()
            try:
                con.execute(stmt)
            except Exception:  # noqa: BLE001
                continue
            blocks.append("statement ok\n" + stmt + ";")
            continue
        sql, ordered = gen.query()
        started = time.perf_counter()
        try:
            cur = con.execute(sql)
            rows = cur.fetchall()
            header = letters(cur.description)
        except Exception:  # noqa: BLE001 - not valid for DuckDB: not a test
            rejected += 1
            continue
        if len(rows) > 400 or time.perf_counter() - started > 0.1:
            rejected += 1  # an answer too large to keep in a file, or a query too heavy for a debug build
            continue
        accepted += 1
        blocks.append(f"query {header}{'' if ordered else ' rowsort'}\n{sql};")
    with open(out_path, "w") as f:
        f.write(f"# generated by tools/fuzz_sql.py --seeds {seed} --queries {count}: DuckDB's answers\n\n")
        f.write("\n\n".join(blocks) + "\n")
    gen_slt.process(out_path)
    return accepted, rejected


def parse_seeds(text):
    if "-" in text:
        lo, hi = text.split("-")
        return list(range(int(lo), int(hi) + 1))
    return [int(s) for s in text.split(",")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", default="1-4", help="e.g. 1-4 or 3,7,9")
    ap.add_argument("--queries", type=int, default=300)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "data", "sqlfuzz"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for seed in parse_seeds(args.seeds):
        path = os.path.join(args.out, f"seed{seed}.test")
        ok, bad = generate(seed, args.queries, path)
        print(f"seed {seed}: {ok} queries ({bad} candidates rejected by DuckDB) -> {path}")


if __name__ == "__main__":
    main()
