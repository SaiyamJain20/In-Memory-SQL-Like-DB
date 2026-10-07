"""Loading and statistics for the comparison campaign (docs/REPORT.md).

A *campaign file* (results/<stage>.jsonl) holds one JSON record per visit. A visit is one fresh
worker process of one engine: load, then every query cold + warm. The unit of statistics is the
**round** (one visit per engine per round, in rotating order): a burst of background load hits one
round of one engine, so rounds - not individual runs - are what is resampled.
"""
import json
import os

import common

ENGINE_ORDER = ["cdb", "duckdb", "datafusion", "chdb", "polars", "sqlite", "duckdb-decimal", "chdb-mergetree"]
NAMES = {"cdb": "cdb", "duckdb": "DuckDB", "datafusion": "DataFusion", "chdb": "ClickHouse (chDB)",
         "polars": "Polars", "sqlite": "SQLite", "duckdb-decimal": "DuckDB (DECIMAL)",
         "chdb-mergetree": "ClickHouse (MergeTree)"}
# fixed categorical slots of the validated reference palette (one colour per engine everywhere)
COLORS = {"cdb": "#2a78d6", "duckdb": "#eb6834", "datafusion": "#1baf7a", "chdb": "#eda100",
          "polars": "#e87ba4", "sqlite": "#008300", "duckdb-decimal": "#eb6834", "chdb-mergetree": "#eda100"}


def load_visits(path):
    if not os.path.exists(path):
        return []
    return [json.loads(line) for line in open(path) if line.strip()]


def choose(visits):
    """One visit per (engine, round): the last attempt that carries no flags; if every attempt was
    flagged, the one with the least background CPU (marked). Returns {(engine, round): visit}."""
    groups = {}
    for v in visits:
        if "skipped" in v:
            continue
        groups.setdefault((v["engine"], v["round"]), []).append(v)
    chosen = {}
    for key, attempts in groups.items():
        clean = [a for a in attempts if not a.get("flags")]
        if clean:
            pick = clean[-1]
            pick["_flagged"] = False
        else:
            pick = min(attempts, key=lambda a: a.get("background_cores", 0))
            pick["_flagged"] = True
        pick["_attempts"] = len(attempts)
        chosen[key] = pick
    return chosen


class Cell:
    """The samples of one query on one engine, per round."""

    def __init__(self):
        self.state = "ok"
        self.round_median = {}  # round -> median warm ms
        self.round_cold = {}
        self.round_cpu = {}  # round -> median warm cpu ms
        self.warm_all = []
        self.rows = None
        self.detail = ""

    def add(self, rnd, entry):
        if entry.get("na"):
            self.state = "na"
            return
        if entry.get("dnf"):
            self.state = "dnf"
            self.detail = f"> {entry.get('timeout_s', '?')} s"
            return
        if entry.get("error"):
            self.state = "error"
            self.detail = entry["error"]
            return
        warm = entry.get("warm_ms") or [entry["cold_ms"]]
        cpu = entry.get("warm_cpu_ms") or [entry.get("cold_cpu_ms", float("nan"))]
        self.round_median[rnd] = common.median(warm)
        self.round_cpu[rnd] = common.median(cpu)
        self.round_cold[rnd] = entry["cold_ms"]
        self.warm_all += warm
        self.rows = entry.get("rows")

    @property
    def ok(self):
        return self.state == "ok" and bool(self.round_median)

    def median(self):
        return common.median(list(self.round_median.values()))

    def ci(self):
        return common.bootstrap_ci(list(self.round_median.values()))

    def best(self):
        return min(self.warm_all)

    def cold(self):
        return common.median(list(self.round_cold.values()))

    def cpu(self):
        return common.median(list(self.round_cpu.values()))

    def spread(self):
        """Relative interquartile range of the per-round medians."""
        v = list(self.round_median.values())
        return common.iqr(v) / common.median(v) if len(v) >= 2 else float("nan")


class Stage:
    """All the visits of one campaign file, organised by engine / query."""

    def __init__(self, path):
        self.path = path
        self.visits = load_visits(path)
        self.chosen = choose(self.visits)
        self.engines = [e for e in ENGINE_ORDER if any(k[0] == e for k in self.chosen)]
        self.rounds = sorted({k[1] for k in self.chosen})
        self.cells = {}
        for (engine, rnd), v in self.chosen.items():
            for q, entry in v["queries"].items():
                self.cells.setdefault((engine, q), Cell()).add(rnd, entry)
        self.queries = sorted({q for (_, q) in self.cells}, key=lambda s: (len(s), s))

    def cell(self, engine, q):
        return self.cells.get((engine, q))

    def visit_values(self, engine, key):
        return [v[key] for (e, _), v in sorted(self.chosen.items()) if e == engine and key in v]

    def flagged_visits(self):
        return sum(1 for v in self.chosen.values() if v["_flagged"])

    def retried_attempts(self):
        return sum(1 for v in self.visits if v.get("attempt", 0) > 0)

    def common_queries(self, engines):
        """Queries every one of `engines` finished (state ok, in every round)."""
        out = []
        for q in self.queries:
            if all(self.cell(e, q) and self.cell(e, q).ok for e in engines):
                out.append(q)
        return out

    def geomean_by_round(self, engine, queries):
        """round -> geometric mean (ms) over `queries` of the engine's median warm time."""
        out = {}
        for r in self.rounds:
            vals = []
            for q in queries:
                c = self.cell(engine, q)
                if c is None or r not in c.round_median:
                    vals = None
                    break
                vals.append(c.round_median[r])
            if vals:
                out[r] = common.geomean(vals)
        return out

    def geomean(self, engine, queries):
        g = self.geomean_by_round(engine, queries)
        v = list(g.values())
        return (common.median(v), *common.bootstrap_ci(v)) if v else (float("nan"),) * 3

    def ratio_geomean(self, engine, ref, queries):
        """Per-round ratio of geometric means engine / ref: median and 95% bootstrap interval."""
        a, b = self.geomean_by_round(engine, queries), self.geomean_by_round(ref, queries)
        v = [a[r] / b[r] for r in a if r in b]
        return (common.median(v), *common.bootstrap_ci(v)) if v else (float("nan"),) * 3

    def ratio_query(self, engine, ref, q):
        ca, cb = self.cell(engine, q), self.cell(ref, q)
        if not (ca and cb and ca.ok and cb.ok):
            return None
        v = [ca.round_median[r] / cb.round_median[r] for r in ca.round_median if r in cb.round_median]
        return (common.median(v), *common.bootstrap_ci(v)) if v else None


def noise_summary(paths):
    """The measured noise floor: for each (engine, query, threads) repeated alone on the machine as
    it was, the relative spread of single runs. `tau` is the 90th percentile of the (p10..p90)/median
    spread over all one-thread groups (and over the 16-thread groups): ratios smaller than this are
    ties."""
    rows = []
    for p in paths:
        stage = os.path.basename(p).replace("noise_", "").replace(".jsonl", "")
        groups = {}
        for line in open(p):
            r = json.loads(line)
            groups.setdefault((r["engine"], r["query"], r["threads"]), []).append(r)
        for (engine, q, threads), v in groups.items():
            w = [x["wall_ms"] for x in v]
            m = common.median(w)
            rows.append({"stage": stage, "engine": engine, "query": q, "threads": threads, "n": len(w),
                         "median_ms": m, "cv": (sum((x - sum(w) / len(w)) ** 2 for x in w) / len(w)) ** 0.5 / (sum(w) / len(w)),
                         "spread": (common.quantile(w, 0.9) - common.quantile(w, 0.1)) / m,
                         "max_over_median": max(w) / m,
                         "freq_avg_mhz": sum(x["probe"]["freq_avg"] for x in v) / len(v),
                         "load1": sum(x["probe"]["load1"] for x in v) / len(v)})
    tau = {}
    for t in (1, 16):
        s = [r["spread"] for r in rows if r["threads"] == t]
        tau[t] = common.quantile(s, 0.9) if s else float("nan")
    return rows, tau
