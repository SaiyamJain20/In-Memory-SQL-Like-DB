"""Shared pieces of the cross-engine comparison harness (docs/REPORT.md, bench/report/README.md).

Table definitions in one neutral form, result comparison against DuckDB's answer files, the
control probes that quantify how noisy the machine is, and the statistics used by the report.
"""
import csv
import glob
import io
import json
import math
import os
import random
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REPORT_DIR = os.path.join(ROOT, "bench", "report")
TPCH_QUERIES = os.path.join(ROOT, "bench", "tpch", "queries")

# Neutral column types: i32, i64, f64, date, str. Every engine maps them to its own types; the
# money columns of TPC-H are f64 (as in cdb, docs/adr/0003-types.md) for every engine.
TPCH_TABLES = {
    "nation": [("n_nationkey", "i32"), ("n_name", "str"), ("n_regionkey", "i32"), ("n_comment", "str")],
    "region": [("r_regionkey", "i32"), ("r_name", "str"), ("r_comment", "str")],
    "part": [("p_partkey", "i32"), ("p_name", "str"), ("p_mfgr", "str"), ("p_brand", "str"),
             ("p_type", "str"), ("p_size", "i32"), ("p_container", "str"), ("p_retailprice", "f64"),
             ("p_comment", "str")],
    "supplier": [("s_suppkey", "i32"), ("s_name", "str"), ("s_address", "str"), ("s_nationkey", "i32"),
                 ("s_phone", "str"), ("s_acctbal", "f64"), ("s_comment", "str")],
    "partsupp": [("ps_partkey", "i32"), ("ps_suppkey", "i32"), ("ps_availqty", "i32"),
                 ("ps_supplycost", "f64"), ("ps_comment", "str")],
    "customer": [("c_custkey", "i32"), ("c_name", "str"), ("c_address", "str"), ("c_nationkey", "i32"),
                 ("c_phone", "str"), ("c_acctbal", "f64"), ("c_mktsegment", "str"), ("c_comment", "str")],
    "orders": [("o_orderkey", "i32"), ("o_custkey", "i32"), ("o_orderstatus", "str"),
               ("o_totalprice", "f64"), ("o_orderdate", "date"), ("o_orderpriority", "str"),
               ("o_clerk", "str"), ("o_shippriority", "i32"), ("o_comment", "str")],
    "lineitem": [("l_orderkey", "i32"), ("l_partkey", "i32"), ("l_suppkey", "i32"),
                 ("l_linenumber", "i32"), ("l_quantity", "f64"), ("l_extendedprice", "f64"),
                 ("l_discount", "f64"), ("l_tax", "f64"), ("l_returnflag", "str"),
                 ("l_linestatus", "str"), ("l_shipdate", "date"), ("l_commitdate", "date"),
                 ("l_receiptdate", "date"), ("l_shipinstruct", "str"), ("l_shipmode", "str"),
                 ("l_comment", "str")],
}
TPCH_ORDER = ["nation", "region", "part", "supplier", "partsupp", "customer", "orders", "lineitem"]


def tpch_spec(sf):
    """The load specification of TPC-H at scale factor `sf` (the CSV files of tools/tpch_data.py)."""
    d = os.path.join(ROOT, "data", f"tpch-sf{sf:g}")
    return {"dataset": f"tpch-sf{sf:g}", "tables": [
        {"name": t, "path": os.path.join(d, f"{t}.csv"), "columns": TPCH_TABLES[t],
         "delimiter": "|", "header": False} for t in TPCH_ORDER]}


def query_file(engine, name):
    """The SQL (or query-module) file of query `name` ('q07') for an engine: its own rewrite when
    bench/report/queries/<engine>/ has one, else the DuckDB-exported text shared by all engines."""
    own = os.path.join(REPORT_DIR, "queries", engine, f"{name}.sql")
    return own if os.path.exists(own) else os.path.join(TPCH_QUERIES, f"{name}.sql")


# ---------------------------------------------------------------------------------- results

def read_pipe_csv(text):
    # a line holding a single NULL is an empty line (csv.reader: no fields); it is one empty field
    return [r or [""] for r in csv.reader(io.StringIO(text), delimiter="|", quotechar='"')]


def write_pipe_csv(path, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f, delimiter="|", quotechar='"', lineterminator="\n")
        for r in rows:
            w.writerow(r)


def cell_text(v):
    """One result value as the text the comparator reads: NULL is empty, floats keep 17 digits."""
    if v is None:
        return ""
    if isinstance(v, float):
        return repr(v)
    if hasattr(v, "isoformat"):
        return v.isoformat()
    return str(v)


def _num(s):
    try:
        return float(s)
    except ValueError:
        return None


def cells_match(got, want, rel):
    if got == want:
        return True
    a, b = _num(got), _num(want)
    if a is None or b is None or got == "" or want == "":
        return False
    return abs(a - b) <= rel * max(1.0, abs(b))


def compare_results(got_path, want_path, rel=1e-7):
    """Compares a dumped result with DuckDB's answer file. Returns (status, detail): 'ok',
    'ok-tie-order' (the same rows, in a different order inside ties of the ORDER BY) or 'mismatch'."""
    got = read_pipe_csv(open(got_path).read())
    want = read_pipe_csv(open(want_path).read())
    if len(got) != len(want):
        return "mismatch", f"{len(got)} rows, expected {len(want)}"
    if any(len(g) != len(w) for g, w in zip(got, want)):
        return "mismatch", "different column counts"
    bad = None
    for i, (g, w) in enumerate(zip(got, want)):
        if not all(cells_match(x, y, rel) for x, y in zip(g, w)):
            bad = i
            break
    if bad is None:
        return "ok", ""

    def key(row):
        return tuple((f"{_num(c):.6g}" if _num(c) is not None and c != "" else c) for c in row)
    if sorted(map(key, got)) == sorted(map(key, want)):
        return "ok-tie-order", f"first difference at row {bad}"
    return "mismatch", f"row {bad}: {got[bad]} vs {want[bad]}"


# ---------------------------------------------------------------------------------- probes

def read_meminfo_available_mb():
    for line in open("/proc/meminfo"):
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) // 1024
    return -1


def read_cpu_freq_mhz():
    freqs = []
    for p in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq"):
        try:
            freqs.append(int(open(p).read()) / 1000.0)
        except OSError:
            pass
    return (min(freqs), sum(freqs) / len(freqs), max(freqs)) if freqs else (0, 0, 0)


def read_proc_stat_busy():
    """(busy jiffies, total jiffies) of the whole machine from /proc/stat."""
    parts = [int(x) for x in open("/proc/stat").readline().split()[1:]]
    idle = parts[3] + parts[4]  # idle + iowait
    total = sum(parts[:8])
    return total - idle, total


def read_proc_cpu_jiffies(pid):
    try:
        s = open(f"/proc/{pid}/stat").read()
    except OSError:
        return 0
    f = s[s.rindex(")") + 2:].split()
    return int(f[11]) + int(f[12])  # utime + stime (+ children are not counted)


def read_loadavg():
    f = open("/proc/loadavg").read().split()
    return float(f[0]), int(f[3].split("/")[0])


def control_probe(cpu_iters=3_000_000, mem_mb=192):
    """Two fixed workloads whose speed moves with CPU frequency and with other processes taking
    the cores or the memory bus: a pure interpreter loop (CPU bound) and a large array copy
    (memory bound). Seconds; a visit is only comparable to others when these agree."""
    t = time.perf_counter()
    x = 0
    for i in range(cpu_iters):
        x = (x * 31 + i) & 0xFFFFFFFF
    cpu_s = time.perf_counter() - t
    import numpy as np
    a = np.ones(mem_mb * 1024 * 1024 // 8)
    b = np.empty_like(a)
    t = time.perf_counter()
    for _ in range(3):
        np.copyto(b, a)
        a, b = b, a
    mem_s = (time.perf_counter() - t) / 3
    load1, running = read_loadavg()
    fmin, favg, fmax = read_cpu_freq_mhz()
    return {"cpu_s": cpu_s, "mem_s": mem_s, "load1": load1, "running": running,
            "mem_available_mb": read_meminfo_available_mb(), "freq_min": fmin, "freq_avg": favg,
            "freq_max": fmax, "t": time.time()}


# ---------------------------------------------------------------------------------- statistics

def median(xs):
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def quantile(xs, q):
    s = sorted(xs)
    if not s:
        return float("nan")
    pos = q * (len(s) - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def iqr(xs):
    return quantile(xs, 0.75) - quantile(xs, 0.25)


def bootstrap_ci(xs, stat=median, n=2000, alpha=0.05, seed=1):
    """Percentile bootstrap interval of `stat` over the samples (resampling whole samples; the
    report's unit of resampling is a round, so that a burst of background load counts once)."""
    if len(xs) < 2:
        return (xs[0], xs[0]) if xs else (float("nan"), float("nan"))
    rng = random.Random(seed)
    vals = sorted(stat([rng.choice(xs) for _ in xs]) for _ in range(n))
    return vals[int(alpha / 2 * n)], vals[int((1 - alpha / 2) * n) - 1]


def geomean(xs):
    xs = [x for x in xs if x is not None and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")


def load_jsonl(path):
    return [json.loads(line) for line in open(path) if line.strip()]
