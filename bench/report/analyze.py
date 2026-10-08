#!/usr/bin/env python3
"""Turns the raw campaign files into the tables, charts and numbers of docs/REPORT.md.

    analyze.py --results bench/report/results/2026-10-08 --out docs/report

Writes <out>/tables/*.md (Markdown tables), <out>/*.svg (charts) and <out>/numbers.json (every
number the prose of the report quotes). Nothing in the report is typed by hand from a result:
bench/report/make_report.py fills the template from these files.
"""
import argparse
import glob
import json
import math
import re
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analysis_lib as A  # noqa: E402
import common  # noqa: E402

NUM = {}  # key -> value, written to numbers.json
TEXT = {}  # key -> the text the report prints
TABLES = {}
TAUS = []  # [configuration, threads, tie band]


def num(key, value, text=None):
    NUM[key] = value
    if text is None:
        text = f"{value:,}" if isinstance(value, int) else (f"{value:.2f}" if isinstance(value, float) else str(value))
    TEXT[key] = text


def fmt_ms(x):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "n/a"
    if x >= 1000:
        return f"{x:,.0f}"
    if x >= 100:
        return f"{x:.0f}"
    if x >= 10:
        return f"{x:.1f}"
    return f"{x:.2f}"


def label(q):
    return q.upper() if q[0] == "q" and q[1:].isdigit() else q


def fmt_ratio(r, tau=None):
    if r is None:
        return "n/a"
    med, lo, hi = r
    tie = (lo <= 1.0 <= hi) or (tau is not None and abs(med - 1) < tau)
    return f"{med:.2f}" + (" ≈" if tie else "")


def put_table(name, header, rows, align=None):
    align = align or ["l"] + ["r"] * (len(header) - 1)
    sep = ["---:" if a == "r" else ":---" for a in align]
    lines = ["| " + " | ".join(header) + " |", "| " + " | ".join(sep) + " |"]
    for r in rows:
        lines.append("| " + " | ".join(str(c) for c in r) + " |")
    TABLES[name] = "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------- TPC-H tables

def correctness(results_dir, sf, threads):
    """{(engine, query): status} from the verification run of this scale factor and thread count
    (verify_sf<sf>_t<threads>.json); empty if there was none."""
    out = {}
    for p in glob.glob(os.path.join(results_dir, f"verify_sf{sf:g}_t{threads}.json")):
        for engine, qs in json.load(open(p)).items():
            for q, v in qs.items():
                out[(engine, q)] = v["status"]
    return out


def stage_tau(st, ref="duckdb"):
    """The tie band of a configuration: the 90th percentile, over queries and engines, of the relative
    interquartile range of the per-round ratio engine / ref. It is the measured noise of the very
    statistic that is compared (two engines measured minutes apart in each round), taken under the
    conditions of this campaign."""
    spreads = []
    for e in st.engines:
        if e in (ref, "sqlite"):
            continue
        for q in st.queries:
            ca, cb = st.cell(e, q), st.cell(ref, q)
            if ca and cb and ca.ok and cb.ok:
                v = [ca.round_median[r] / cb.round_median[r] for r in ca.round_median if r in cb.round_median]
                if len(v) >= 3:
                    spreads.append(common.iqr(v) / common.median(v))
    return common.quantile(spreads, 0.9) if spreads else 0.1


def tpch_stage_tables(st, name, ref, tau, bad):
    """Per-query medians (ms) and ratios to `ref` for every engine of the stage."""
    engines = st.engines
    qs = st.queries
    header = ["Query"] + [A.NAMES[e] for e in engines]
    rows = []
    for q in qs:
        row = [label(q)]
        for e in engines:
            c = st.cell(e, q)
            if c is None:
                row.append("–")
            elif c.state == "dnf":
                row.append(f"DNF ({c.detail})")
            elif c.state == "na":
                row.append("n/a")
            elif c.state == "error":
                row.append("error")
            else:
                mark = " ✗" if bad.get((e, q), "ok") not in ("ok", "ok-tie-order") else ""
                row.append(fmt_ms(c.median()) + mark)
                NUM[f"ms_{name}_{e}_{q}"] = c.median()
        rows.append(row)
    common_qs = st.common_queries(engines)
    row = ["**geometric mean** (ms)"]
    for e in engines:
        row.append(f"**{fmt_ms(st.geomean(e, common_qs)[0])}**")
    rows.append(row)
    put_table(f"{name}_ms", header, rows)

    rows = []
    for q in qs:
        row = [label(q)]
        for e in engines:
            if e == ref:
                row.append("1")
                continue
            r = st.ratio_query(e, ref, q)
            row.append(fmt_ratio(r, tau))
            if r:
                NUM[f"r_{name}_{e}_{q}"] = r[0]
        rows.append(row)
    row = ["**geometric mean**"]
    for e in engines:
        if e == ref:
            row.append("**1**")
            continue
        med, lo, hi = st.ratio_geomean(e, ref, common_qs)
        row.append(f"**{med:.2f}** [{lo:.2f}, {hi:.2f}]")
    rows.append(row)
    put_table(f"{name}_ratio", header, rows)
    return common_qs


# ---------------------------------------------------------------------------------- charts

def setup_matplotlib():
    import matplotlib
    matplotlib.use("svg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({
        "svg.fonttype": "none", "font.family": "DejaVu Sans", "font.size": 9, "axes.edgecolor": "#c9c8c2",
        "axes.linewidth": 0.8, "axes.facecolor": "#fcfcfb", "figure.facecolor": "#fcfcfb",
        "axes.labelcolor": "#52514e", "xtick.color": "#52514e", "ytick.color": "#52514e",
        "text.color": "#0b0b0b", "axes.spines.top": False, "axes.spines.right": False,
        "grid.color": "#e6e5e0", "grid.linewidth": 0.8, "axes.grid": True, "axes.axisbelow": True})
    return plt


def save(plt, fig, path):
    fig.savefig(path)
    if os.environ.get("CHART_PNG"):  # a raster copy for looking at the chart
        fig.savefig(path.replace(".svg", ".png"), dpi=110)
    plt.close(fig)


def plain_log_axis(ax, which, ticks):
    from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator
    axis = ax.xaxis if which == "x" else ax.yaxis
    axis.set_major_locator(FixedLocator(ticks))
    axis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    axis.set_minor_locator(NullLocator())


def chart_dot_ratio(plt, path, st, ref, queries, title, tau=None, xlabel=None):
    """One row per query, one dot per engine at its time / the reference's time (log axis)."""
    engines = [e for e in st.engines if e != ref and e in ("cdb", "datafusion", "chdb", "polars", "sqlite")]
    fig, ax = plt.subplots(figsize=(7.2, 0.28 * len(queries) + 1.6))
    ys = list(range(len(queries)))
    ax.axvline(1.0, color="#52514e", lw=1.2, zorder=1)
    if tau:
        ax.axvspan(1 - tau, 1 + tau, color="#e6e5e0", zorder=0, alpha=0.7)
    for k, e in enumerate(engines):
        xs, yy = [], []
        for i, q in enumerate(queries):
            r = st.ratio_query(e, ref, q)
            if r:
                xs.append(r[0])
                yy.append(i + (k - (len(engines) - 1) / 2) * 0.13)  # a little vertical room per engine
        ax.scatter(xs, yy, s=30, color=A.COLORS[e], edgecolor="#fcfcfb", linewidth=1.2, zorder=3, label=A.NAMES[e])
    ax.set_xscale("log")
    plain_log_axis(ax, "x", [t for t in (0.1, 0.25, 0.5, 1, 2, 4, 10, 25, 100, 250) if 0.07 <= t <= 400])
    ax.set_yticks(ys)
    ax.set_yticklabels([label(q) for q in queries])
    ax.invert_yaxis()
    ax.set_xlabel(xlabel or f"time relative to {A.NAMES[ref]} (log scale; left of the line = faster)")
    ax.set_title(title, loc="left", fontsize=10, color="#0b0b0b")
    ax.grid(axis="y", visible=False)
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, -0.0), ncol=len(engines), frameon=False,
              bbox_transform=fig.transFigure, fontsize=8)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    save(plt, fig, path)


def chart_geomean(plt, path, panels, ref, title):
    """panels: [(label, stage, queries)]; bars of the geometric-mean ratio to `ref` with 95% intervals."""
    fig, axes = plt.subplots(1, len(panels), figsize=(max(6.6, 3.6 * len(panels)), 3.4), sharey=False)
    if len(panels) == 1:
        axes = [axes]
    for ax, (label, st, qs) in zip(axes, panels):
        engines = [e for e in st.engines if e in ("cdb", "duckdb", "datafusion", "chdb", "polars", "sqlite")]
        for i, e in enumerate(engines):
            med, lo, hi = (1.0, 1.0, 1.0) if e == ref else st.ratio_geomean(e, ref, qs)
            ax.bar(i, med, width=0.6, color=A.COLORS[e], zorder=2)
            ax.errorbar(i, med, yerr=[[max(0, med - lo)], [max(0, hi - med)]], color="#0b0b0b", lw=1, capsize=3, zorder=3)
            ax.text(i, hi * 1.12, f"{med:.2f}", ha="center", va="bottom", fontsize=8, color="#0b0b0b")
        ax.set_yscale("log")
        top = max(1.5, ax.get_ylim()[1])
        plain_log_axis(ax, "y", [t for t in (0.5, 1, 2, 5, 10, 20, 50, 100) if t <= top * 1.2])
        ax.axhline(1.0, color="#52514e", lw=1)
        ax.set_xticks(range(len(engines)))
        ax.set_xticklabels([A.NAMES[e].replace(" (chDB)", "") for e in engines], rotation=30, ha="right")
        ax.set_title(label, loc="left", fontsize=9)
        ax.grid(axis="x", visible=False)
    axes[0].set_ylabel(f"geometric-mean time / {A.NAMES[ref]}")
    fig.suptitle(title, x=0.01, ha="left", fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    save(plt, fig, path)


def chart_scaling(plt, path, by_threads, engines, title):
    """by_threads: {threads: Stage}; speedup of the geometric-mean time over one thread."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.4, 3.3))
    ts = sorted(by_threads)
    base = by_threads[ts[0]]
    qs = None
    for e in engines:
        stages = [by_threads[t] for t in ts if e in by_threads[t].engines]
        if len(stages) != len(ts):
            continue
        qs_e = stages[0].common_queries([e])
        for st in stages[1:]:
            qs_e = [q for q in qs_e if st.cell(e, q) and st.cell(e, q).ok]
        g = [st.geomean(e, qs_e)[0] for st in stages]
        ax1.plot(ts, g, marker="o", ms=5, lw=2, color=A.COLORS[e], label=A.NAMES[e].replace(" (chDB)", ""),
                 markeredgecolor="#fcfcfb", markeredgewidth=1.2)
        ax2.plot(ts, [g[0] / x for x in g], marker="o", ms=5, lw=2, color=A.COLORS[e], markeredgecolor="#fcfcfb",
                 markeredgewidth=1.2)
    ax2.plot(ts, ts, color="#52514e", lw=1, ls=(0, (1, 2)), label="ideal")
    for ax in (ax1, ax2):
        ax.set_xscale("log", base=2)
        ax.set_xticks(ts)
        ax.set_xticklabels([str(t) for t in ts])
        ax.set_xlabel("threads")
    ax1.set_yscale("log")
    plain_log_axis(ax1, "y", [20, 30, 50, 70, 100, 150])
    ax1.set_ylabel("geometric-mean query time (ms)")
    ax2.set_ylabel("speedup over one thread")
    ax1.legend(frameon=False, fontsize=8)
    fig.suptitle(title, x=0.01, ha="left", fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    save(plt, fig, path)


# ---------------------------------------------------------------------------------- more tables and charts

def environment(res):
    """What the machine was doing during the campaign, from every visit's probes."""
    chosen, attempts, probes = [], 0, []
    for p in sorted(glob.glob(os.path.join(res, "*.jsonl"))):
        if os.path.basename(p).startswith("noise_"):
            continue
        visits = A.load_visits(p)
        attempts += len(visits)
        num_retried = sum(1 for v in visits if v.get("attempt", 0) > 0)
        NUM["_retried"] = NUM.get("_retried", 0) + num_retried
        st = A.Stage(p)
        chosen += list(st.chosen.values())
        NUM["_flagged_final"] = NUM.get("_flagged_final", 0) + st.flagged_visits()
        for v in visits:
            if "probe_before" in v:
                probes += [v["probe_before"], v["probe_after"]]
    bg = sorted(v["background_cores"] for v in chosen if "background_cores" in v)
    if not bg:
        return
    num("bg_median", common.median(bg), f"{common.median(bg):.1f}")
    num("bg_p90", common.quantile(bg, 0.9), f"{common.quantile(bg, 0.9):.1f}")
    num("bg_max", bg[-1], f"{bg[-1]:.1f}")
    num("freq_mean_ghz", sum(p["freq_avg"] for p in probes) / len(probes) / 1000, f"{sum(p['freq_avg'] for p in probes) / len(probes) / 1000:.1f}")
    num("freq_min_ghz", min(p["freq_min"] for p in probes) / 1000, f"{min(p['freq_min'] for p in probes) / 1000:.1f}")
    num("freq_max_ghz", max(p["freq_max"] for p in probes) / 1000, f"{max(p['freq_max'] for p in probes) / 1000:.1f}")
    mem = [p["mem_available_mb"] for p in probes]
    num("mem_available_min_gb", min(mem) / 1024, f"{min(mem) / 1024:.0f}")
    num("mem_available_max_gb", max(mem) / 1024, f"{max(mem) / 1024:.0f}")
    num("visits_total", attempts)
    num("visits_retried", NUM.pop("_retried", 0))
    num("visits_flagged_final", NUM.pop("_flagged_final", 0))


def codebase_numbers():
    def lines(globs):
        n = 0
        for g in globs:
            for p in glob.glob(os.path.join(common.ROOT, g), recursive=True):
                try:
                    n += sum(1 for _ in open(p, errors="ignore"))
                except OSError:
                    pass
        return n
    num("loc_src", lines(["src/**/*.cpp", "src/**/*.h"]))
    num("loc_tests", lines(["tests/**/*.cpp", "tests/**/*.h"]))
    num("loc_harness", lines(["bench/report/*.py", "bench/report/*.cpp", "bench/report/*.sh", "bench/report/queries/sqlite/*.sql"]))
    try:
        sys.path.insert(0, os.path.join(common.ROOT, "tools"))
        import importlib.util
        spec = importlib.util.spec_from_file_location("mutation_smoke", os.path.join(common.ROOT, "tools", "mutation_smoke.py"))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        num("mutants", len(m.MUTATIONS))
    except Exception:
        num("mutants", 319)
    num("test_count", 976)  # the last gate: debug / asan / tsan / clang-18 (release: 974, two cases are skipped there)
    import workloads
    num("micro_queries", len(workloads.MICRO_QUERIES))


def standing_one(st, key, tt):
    """Where cdb stands against each engine of a stage: geometric-mean ratio, and how many queries it
    wins, ties or loses (a win or loss needs the 95% interval to exclude 1 and a gap larger than the
    tie band); and which engine is fastest on how many queries."""
    if "cdb" not in st.engines:
        return
    rows = []
    for e in st.engines:
        if e == "cdb":
            continue
        qs = st.common_queries(["cdb", e])
        if not qs:
            continue
        med, lo, hi = st.ratio_geomean("cdb", e, qs)
        win = tie = loss = 0
        for q in qs:
            r = st.ratio_query("cdb", e, q)
            if r is None:
                continue
            m, l, h = r
            if (l <= 1.0 <= h) or abs(m - 1) < tt:
                tie += 1
            elif m < 1:
                win += 1
            else:
                loss += 1
        rows.append([A.NAMES[e], len(qs), f"{med:.2f} [{lo:.2f}, {hi:.2f}]", win, tie, loss])
        NUM[f"standing_{key}_{e}_ratio"], NUM[f"standing_{key}_{e}_win"] = med, win
        NUM[f"standing_{key}_{e}_tie"], NUM[f"standing_{key}_{e}_loss"] = tie, loss
    if rows:
        put_table(f"standing_{key}", ["cdb against", "Queries", "cdb time / their time (geometric mean) [95% interval]",
                                      "cdb faster", "tie", "cdb slower"], rows)
    counts = {e: 0 for e in st.engines}
    for q in st.queries:
        vals = {e: st.cell(e, q).median() for e in st.engines if st.cell(e, q) and st.cell(e, q).ok}
        if len(vals) >= 2:
            counts[min(vals, key=vals.get)] += 1
    put_table(f"fastest_{key}", ["Engine", "Queries on which it is fastest"], [[A.NAMES[e], counts[e]] for e in st.engines])
    for e in st.engines:
        NUM[f"fastest_{key}_{e}"] = counts[e]


def standing_tables(stages, tau):
    for (sf, t), st in sorted(stages.items()):
        standing_one(st, f"sf{sf:g}_t{t}", getattr(st, "tau", None) or stage_tau(st))


def load_memory_table(res, stages):
    rows = []
    for t in (1, 16):
        st = stages.get((1, t))
        if not st:
            continue
        for e in st.engines:
            load = st.visit_values(e, "load_wall_s")
            hwm = st.visit_values(e, "hwm_kb")
            hwm_load = st.visit_values(e, "hwm_after_load_kb")
            stored = st.visit_values(e, "stored_bytes")
            if not load:
                continue
            rows.append([A.NAMES[e], t, f"{common.median(load):.1f}", f"{common.median(hwm_load) / 1024:,.0f}",
                         f"{common.median(hwm) / 1024:,.0f}",
                         f"{common.median(stored) / 1048576:,.0f}" if stored else "–"])
            if e == "cdb" and t == 1:
                num("stored_mb_sf1", common.median(stored) / 1048576, f"{common.median(stored) / 1048576:,.0f}")
                num("stored_ratio", 1408 / (common.median(stored) / 1048576), f"{1408 / (common.median(stored) / 1048576):.1f}")
            NUM[f"load_{e}_t{t}"] = common.median(load)
            NUM[f"rss_{e}_t{t}"] = common.median(hwm) / 1024
    if "load_cdb_t1" in NUM and "load_cdb_t16" in NUM:
        num("load_ratio_cdb", NUM["load_cdb_t1"] / NUM["load_cdb_t16"], f"{NUM['load_cdb_t1'] / NUM['load_cdb_t16']:.1f}")
    if rows:
        put_table("load_memory_sf1", ["Engine", "Threads", "Load, all 8 tables (s)", "Peak RSS after load (MB)",
                                      "Peak RSS after all queries (MB)", "cdb storage (MB)"], rows)


def efficiency_table(stages):
    for t in (1, 16):  # the effective parallelism at the small scale factor, for the discussion of 6.4
        st = stages.get((0.1, t))
        if st:
            for e in st.engines:
                qs = st.common_queries([e])
                NUM[f"par_{e}_sf0.1_t{t}"] = sum(st.cell(e, q).cpu() for q in qs) / sum(st.cell(e, q).median() for q in qs)
    rows = []
    for t in (1, 16):
        st = stages.get((1, t))
        if not st:
            continue
        for e in st.engines:
            qs = st.common_queries([e])
            wall = sum(st.cell(e, q).median() for q in qs)
            cpu = sum(st.cell(e, q).cpu() for q in qs)
            rows.append([A.NAMES[e], t, len(qs), f"{wall / 1000:.2f}", f"{cpu / 1000:.2f}", f"{cpu / wall:.1f}"])
            NUM[f"cpu_s_{e}_t{t}"] = cpu / 1000
            if e == "duckdb" and f"cpu_s_cdb_t{t}" in NUM:
                num(f"cpu_ratio_t{t}", NUM[f"cpu_s_cdb_t{t}"] / (cpu / 1000), f"{NUM[f'cpu_s_cdb_t{t}'] / (cpu / 1000):.1f}")
            NUM[f"par_{e}_t{t}"] = cpu / wall
    if rows:
        put_table("efficiency_sf1", ["Engine", "Threads", "Queries", "Sum of wall times (s)", "Sum of CPU times (s)",
                                     "Effective parallelism (CPU / wall)"], rows)


def sf_scaling_table(stages):
    rows = []
    for t in (1, 16):
        have = [sf for sf in (0.1, 1, 3) if (sf, t) in stages]
        if len(have) < 2:
            continue
        engines = [e for e in A.ENGINE_ORDER[:6] if all(e in stages[(sf, t)].engines for sf in have)]
        for e in engines:
            qs = None
            for sf in have:
                c = stages[(sf, t)].common_queries([e])
                qs = c if qs is None else [q for q in qs if q in c]
            g = [stages[(sf, t)].geomean(e, qs)[0] for sf in have]
            rows.append([A.NAMES[e], t] + [fmt_ms(x) for x in g] + [f"{g[-1] / g[0]:.1f}x over {have[-1] / have[0]:g}x the data"])
            NUM[f"growth_{e}_t{t}"] = g[-1] / g[0]
    if rows:
        put_table("sf_scaling", ["Engine", "Threads"] + [f"SF{sf:g} (ms)" for sf in (0.1, 1, 3)][:len(rows[0]) - 3] + ["Growth"], rows)


def cold_warm_table(stages):
    rows = []
    st = stages.get((1, 1))
    if not st:
        return
    for e in st.engines:
        qs = st.common_queries([e])
        cold = common.geomean([st.cell(e, q).cold() for q in qs])
        warm = common.geomean([st.cell(e, q).median() for q in qs])
        rows.append([A.NAMES[e], fmt_ms(cold), fmt_ms(warm), f"{cold / warm:.2f}"])
        NUM[f"coldwarm_{e}"] = cold / warm
    put_table("cold_warm_sf1_t1", ["Engine", "Cold (geometric mean, ms)", "Warm (ms)", "Cold / warm"], rows)


def correctness_table(res):
    out = {}
    for p in sorted(glob.glob(os.path.join(res, "verify_sf*_t*.json")) + glob.glob(os.path.join(res, "check_*_t*.json"))):
        name = os.path.basename(p).replace("verify_", "").replace("check_", "")[:-5]
        for engine, qs in json.load(open(p)).items():
            ok = sum(1 for v in qs.values() if v["status"] in ("ok", "ok-tie-order"))
            na = sum(1 for v in qs.values() if v["status"] == "n/a")
            bad = [f"{q.upper()} ({v['status']}: {v['detail'][:40]})" for q, v in qs.items()
                   if v["status"] not in ("ok", "ok-tie-order", "n/a")]
            out.setdefault((engine), {})[name] = (ok, len(qs) - na, bad)
    if not out:
        return
    names = sorted({n for v in out.values() for n in v})
    rows = []
    for e in A.ENGINE_ORDER:
        if e not in out:
            continue
        row = [A.NAMES[e]]
        notes = []
        for n in names:
            if n in out[e]:
                ok, tot, bad = out[e][n]
                row.append(f"{ok} / {tot}")
                notes += [f"{b} at {n}" for b in bad]
            else:
                row.append("–")
        row.append("; ".join(notes) or "all answers match")
        rows.append(row)
    def column_label(n):
        wl, _, t = n.rpartition("_t")
        wl = {"h2o-g1": "H2O groupby", "h2o-j1": "H2O join", "micro": "micro"}.get(wl, "TPC-H " + wl.replace("sf", "SF"))
        return f"{wl}, {t} thr"
    put_table("correctness", ["Engine"] + [column_label(n) for n in names] + ["Not matching the reference answer"],
              rows, ["l"] + ["r"] * len(names) + ["l"])


def workload_stage(res, name, plt, out, tau):
    """Tables and the dot chart of a micro-benchmark / H2O-style workload (1 and 16 threads)."""
    for t in (1, 16):
        st = A.Stage(os.path.join(res, f"{name}_t{t}.jsonl"))
        if not st.visits:
            continue
        tau_t = stage_tau(st)
        st.tau = tau_t
        num(f"tau_{name}_t{t}_pct", tau_t * 100, f"{tau_t * 100:.0f}")
        TAUS.append([{"micro": "operator micro-benchmarks", "h2o-g1": "H2O-style groupby", "h2o-j1": "H2O-style join"}[name], t,
                     f"{tau_t * 100:.0f}%"])
        qs = tpch_stage_tables(st, f"{name}_t{t}", "duckdb", tau_t, {})
        standing_one(st, f"{name}_t{t}", tau_t)
        NUM[f"{name}_t{t}_queries"] = len(qs)
        for e in st.engines:
            if e != "duckdb":
                med, lo, hi = st.ratio_geomean(e, "duckdb", qs)
                NUM[f"{name}_t{t}_ratio_{e}"] = med
        chart_dot_ratio(plt, os.path.join(out, f"{name}_t{t}_ratio.svg"), st, "duckdb", st.queries,
                        f"{name}, {t} thread{'s' if t > 1 else ''}: time per query relative to DuckDB", tau_t)


def variants_table(res, stages):
    rows = []
    for t in (1, 16):
        v = A.Stage(os.path.join(res, f"tpch_sf1_variants_t{t}.jsonl"))
        base = stages.get((1, t))
        if not v.visits or not base:
            continue
        for e, ref in (("duckdb-decimal", "duckdb"), ("chdb-mergetree", "chdb")):
            if e not in v.engines or ref not in base.engines:
                continue
            qs = [q for q in v.common_queries([e]) if base.cell(ref, q) and base.cell(ref, q).ok]
            g_v, g_b = v.geomean(e, qs)[0], base.geomean(ref, qs)[0]
            rows.append([A.NAMES[e], A.NAMES[ref], t, fmt_ms(g_v), fmt_ms(g_b), f"{g_v / g_b:.2f}"])
            NUM[f"variant_{e}_t{t}"] = g_v / g_b
    if rows:
        put_table("variants_sf1", ["Variant", "Compared with", "Threads", "Variant (geometric mean, ms)", "Primary (ms)",
                                   "Variant / primary"], rows)


def optimizer_section(res, plt, out):
    p = os.path.join(res, "optimizer_quality_sf1.json")
    if not os.path.exists(p):
        return
    d = json.load(open(p))
    rows = []
    for kind in ("SCAN", "FILTER", "JOIN", "AGGREGATE", "OTHER", "ALL"):
        row = [kind.title() if kind != "ALL" else "**All**"]
        for e in ("cdb", "duckdb"):
            q = d["engines"][e]["qerror"].get(kind)
            row += [q["n"], f"{q['median']:.2f}", f"{q['p90']:.1f}", f"{q['max']:,.0f}"] if q else ["–"] * 4
        rows.append(row)
    put_table("qerror", ["Operator kind", "cdb n", "median", "90th pct", "max", "DuckDB n", "median", "90th pct", "max"], rows)
    for e in ("cdb", "duckdb"):
        for kind in ("JOIN", "AGGREGATE", "ALL"):
            q = d["engines"][e]["qerror"].get(kind)
            if q:
                num(f"qerr_{e}_{kind.lower()}_median", q["median"])
                num(f"qerr_{e}_{kind.lower()}_p90", q["p90"], f"{q['p90']:.1f}")
                num(f"qerr_{e}_{kind.lower()}_max", q["max"], f"{q['max']:,.0f}")
    c = d["engines"]["cdb"]["c_out"]
    dk = d["engines"]["duckdb"]["c_out"]
    rows = []
    ratios = []
    for q in sorted(c, key=lambda s: (len(s), s)):
        a, b = c[q], dk.get(q, 0)
        r = (max(a, 1) / max(b, 1))
        ratios.append(r)
        NUM[f"r_cout_{q}"] = r
        NUM[f"cout_{q}_cdb"], NUM[f"cout_{q}_duckdb"] = a, b
        rows.append([label(q), f"{a:,}", f"{b:,}", f"{r:.2f}" if a or b else "–"])
    rows.append(["**geometric mean**", "", "", f"**{common.geomean(ratios):.2f}**"])
    put_table("cout", ["Query", "cdb C_out (rows out of all joins)", "DuckDB C_out", "cdb / DuckDB"], rows)
    num("cout_ratio_gm", common.geomean(ratios))
    num("cout_queries_cdb_smaller", sum(1 for r in ratios if r < 0.95))
    num("cout_queries_cdb_larger", sum(1 for r in ratios if r > 1.05))
    # chart: q-error per kind
    fig, ax = plt.subplots(figsize=(6.4, 3.2))
    kinds = ["SCAN", "FILTER", "JOIN", "AGGREGATE", "ALL"]
    w = 0.34
    for k, e in enumerate(("cdb", "duckdb")):
        xs = [i + (k - 0.5) * w for i in range(len(kinds))]
        med = [d["engines"][e]["qerror"].get(kd, {}).get("median", 1) for kd in kinds]
        p90 = [d["engines"][e]["qerror"].get(kd, {}).get("p90", 1) for kd in kinds]
        ax.bar(xs, p90, width=w * 0.9, color=A.COLORS[e], alpha=0.35, zorder=2)
        ax.bar(xs, med, width=w * 0.9, color=A.COLORS[e], zorder=3, label=f"{A.NAMES[e]} (median; lighter bar: 90th percentile)")
        for x, v in zip(xs, p90):
            ax.text(x, v * 1.08, f"{v:,.0f}" if v >= 100 else f"{v:.1f}", ha="center", va="bottom", fontsize=8)
    ax.set_yscale("log")
    ax.set_ylim(0.9, 40000)
    plain_log_axis(ax, "y", [1, 2, 5, 10, 100, 1000, 10000])
    ax.set_xticks(range(len(kinds)))
    ax.set_xticklabels([k.title() for k in kinds])
    ax.set_ylabel("q-error (1 = exact)")
    ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, fontsize=8, loc="upper left")
    ax.set_title("Estimated vs actual rows per operator, 22 TPC-H queries at SF1", loc="left", fontsize=10)
    fig.tight_layout()
    save(plt, fig, os.path.join(out, "qerror.svg"))


def durability_section(res, plt, out):
    p = os.path.join(res, "durability.json")
    if not os.path.exists(p):
        return
    d = json.load(open(p))["engines"]
    rows = []
    for e in ("cdb", "sqlite", "duckdb", "chdb"):
        if e not in d:
            continue
        r = d[e]
        c = r.get("commit (durable)")
        n = r.get("commit (not durable)")
        bulk = r.get("bulk load")
        if bulk is None and r.get("bulk load (log)"):
            # cdb: the load logged and fsynced, plus the checkpoint that turns the log into the files
            bulk = {"s": r["bulk load (log)"]["s"] + r.get("checkpoint", {}).get("s", 0)}
            num("cdb_log_mb", r["bulk load (log)"]["log_mb"], f"{r['bulk load (log)']['log_mb']:,.0f}")
        size = (r.get("bulk load", {}).get("bytes") or r.get("checkpoint", {}).get("bytes") or 0) / 1048576
        rows.append([A.NAMES.get(e, e) if e != "chdb" else "ClickHouse (MergeTree)",
                     f"{c['mean_ms']:.2f} ms / {c['p99_ms']:.2f}" if c else "–",
                     f"{c['per_s']:,.0f}" if c else "–",
                     f"{n['mean_ms']:.3f} ms" if n else "–",
                     f"{bulk['s']:.1f}" if bulk else "–", f"{size:,.0f}" if size else "–",
                     f"{r['reopen']['s']:.2f}" if r.get("reopen") else "–"])
        if bulk:
            num(f"bulk_s_{e}", bulk["s"], f"{bulk['s']:.1f}")
        if size:
            num(f"size_mb_{e}", size, f"{size:,.0f}")
        if c:
            num(f"commit_ms_{e}", c["mean_ms"], f"{c['mean_ms']:.2f}")
            num(f"commit_per_s_{e}", c["per_s"], f"{c['per_s']:,.0f}")
        if r.get("reopen"):
            num(f"reopen_s_{e}", r["reopen"]["s"], f"{r['reopen']['s']:.2f}")
    if "commit_ms_cdb" in NUM and "commit_ms_sqlite" in NUM:
        pct = (NUM["commit_ms_cdb"] / NUM["commit_ms_sqlite"] - 1) * 100
        num("commit_vs_sqlite_pct", pct, f"{pct:.0f}")
    put_table("durability", ["Engine", "Durable commit: mean / p99", "Commits per second", "Not durable: mean",
                             "Bulk load SF1 (s)", "On disk (MB)", "Reopen + Q6 (s)"], rows)


def bandwidth_table(res, stages_micro):
    """The scan-bound micro-benchmarks as a share of what the memory system can deliver (membw.cpp)."""
    p = os.path.join(res, "membw.txt")
    if not os.path.exists(p):
        return
    bw = {}
    for line in open(p):
        parts = line.split()
        if len(parts) == 3 and parts[0].isdigit():
            bw[int(parts[0])] = (float(parts[1]), float(parts[2]))
    if 1 not in bw or 16 not in bw:
        return
    num("membw_read_1", bw[1][0], f"{bw[1][0]:.1f}")
    num("membw_read_16", bw[16][0], f"{bw[16][0]:.1f}")
    if 4 in bw:
        num("membw_read_4", bw[4][0], f"{bw[4][0]:.1f}")
    num("membw_copy_16", bw[16][1], f"{bw[16][1]:.1f}")
    rows = []
    # logical bytes each query must look at: scan_sum reads v (8 B x 10M); filter_* read i (4 B) and v (8 B); scan_expr v and i
    logical = {"scan_sum": 8e7, "scan_expr": 1.2e8, "scan_minmax": 1.2e8, "filter_10pct": 1.2e8, "filter_90pct": 1.2e8}
    for q, nbytes in logical.items():
        for t in (1, 16):
            st = stages_micro.get(t)
            if not st:
                continue
            row = [q, t]
            for e in ("cdb", "duckdb", "datafusion", "chdb", "polars"):
                c = st.cell(e, q)
                if c and c.ok:
                    gbs = nbytes / (c.median() / 1000) / 1e9
                    row.append(f"{gbs:.1f} ({100 * gbs / bw[t][0]:.0f}%)")
                    NUM[f"gbs_{e}_t{t}_{q}"] = gbs
                else:
                    row.append("–")
            rows.append(row)
    put_table("bandwidth", ["Query", "Threads"] + [A.NAMES[e].replace(" (chDB)", "") for e in ("cdb", "duckdb", "datafusion", "chdb", "polars")], rows)


def short_function(name):
    """`???:cdb::KeyComparator::StoredEqualsInput(...) const [path]` -> `cdb::KeyComparator::StoredEqualsInput`."""
    import re
    n = name.split(" [")[0]
    n = n.split(":", 1)[1] if ":" in n and n.split(":", 1)[0] in ("???", "") or n.startswith("./") else n
    n = n.replace("(anonymous namespace)::", "")
    n = re.sub(r"^(void|bool|auto|unsigned long|unsigned int\*?|decltype\(auto\))\s+", "", n)
    n = n.split("(")[0].split("<")[0]
    return n[:64]


def profile_section(res):
    """Instruction counts and simulated last-level data misses of one warm execution (callgrind with a
    cache simulation), cdb against DuckDB, and where cdb's instructions go."""
    d = os.path.join(res, "profiles")
    if not os.path.isdir(d):
        return
    def load(e, q):
        p = os.path.join(d, f"{e}_{q}.json")
        if not os.path.exists(p):
            return None
        j = json.load(open(p))
        ev = j["events"]
        return {"ir": j["totals"][ev.index("Ir")], "dlmr": j["totals"][ev.index("DLmr")], "funcs": j["top_functions"]}
    order = [("topn_10", "micro: top-10 of 1M rows"), ("sort_5m", "micro: sort 1M rows"), ("join_1k", "micro: join, 1,000-row build"),
             ("join_1m", "micro: join, 1M-row build"), ("agg_100k", "micro: 100,000 groups"), ("agg_1m", "micro: 1M groups"),
             ("scan_count", "micro: count(*)"), ("scan_sum", "micro: sum of a column"), ("filter_90pct", "micro: filter 90%, sum"),
             ("q1", "TPC-H Q1, SF0.1"), ("q6", "TPC-H Q6"), ("q9", "TPC-H Q9"), ("q17", "TPC-H Q17"), ("q20", "TPC-H Q20")]
    rows, top = [], []
    for q, label_ in order:
        a, b = load("cdb", q), load("duckdb", q)
        if not a:
            continue
        r = [label_, f"{a['ir'] / 1e6:,.0f}", f"{b['ir'] / 1e6:,.0f}" if b else "–", f"{a['ir'] / b['ir']:.1f}" if b else "–",
             f"{a['dlmr'] / 1e3:,.0f}", f"{b['dlmr'] / 1e3:,.0f}" if b else "–"]
        rows.append(r)
        if b:
            NUM[f"ir_ratio_{q}"] = a["ir"] / b["ir"]
            NUM[f"dlmr_ratio_{q}"] = a["dlmr"] / max(1, b["dlmr"])
        NUM[f"ir_cdb_{q}"], NUM[f"ir_duckdb_{q}"] = a["ir"], b["ir"] if b else 0
        num(f"irm_cdb_{q}", a["ir"] / 1e6, f"{a['ir'] / 1e6:,.0f}")
        if b:
            num(f"irm_duckdb_{q}", b["ir"] / 1e6, f"{b['ir'] / 1e6:,.0f}")
        fn = [f for f in a["funcs"] if f["pct"] >= 4.0][:3]
        top.append([label_] + [f"{f['pct']:.0f}% `{short_function(f['name'])}`" for f in fn] + [""] * (3 - len(fn)))
        for k, f in enumerate(fn):
            NUM[f"top{k}_pct_{q}"] = f["pct"]
    if rows:
        put_table("profile", ["Query", "cdb instructions (M)", "DuckDB (M)", "cdb / DuckDB", "cdb simulated LL read misses (k)",
                              "DuckDB (k)"], rows)
        put_table("profile_top", ["Query", "Largest function of cdb", "Second", "Third"], top, ["l", "l", "l", "l"])


def scaling_probe_section(res):
    rows = []
    for q in (4, 21):
        p = os.path.join(res, f"scaling_q{q:02d}.json")
        if not os.path.exists(p):
            continue
        d = json.load(open(p))
        for t in ("1", "16"):
            r = d[t]
            outside = 1 - r["operator_own_cpu_ms"] / r["process_cpu_ms"]
            rows.append([f"Q{q}", t, f"{r['wall_ms']:.0f}", f"{r['process_cpu_ms']:.0f}", f"{r['operator_own_cpu_ms']:.0f}",
                         f"{100 * outside:.0f}%"])
            NUM[f"scal_q{q}_t{t}_outside"] = 100 * outside
            NUM[f"scal_q{q}_t{t}_wall"] = r["wall_ms"]
            NUM[f"scal_q{q}_t{t}_cpu"] = r["process_cpu_ms"]
            NUM[f"scal_q{q}_t{t}_ops"] = r["operator_own_cpu_ms"]
            for line in r["explain_analyze"]:
                m = re.search(r"SCAN lineitem .*actual \d+ rows, ([0-9.]+) ms", line)
                if m:
                    NUM[f"scal_q{q}_t{t}_scan_lineitem_ms"] = float(m.group(1))
                m = re.search(r"build (\d+) rows", line)
                if m:
                    NUM[f"scal_q{q}_build_rows"] = int(m.group(1))
                    TEXT[f"scal_q{q}_build_rows"] = f"{int(m.group(1)) / 1e6:.1f} million"
        NUM[f"scal_q{q}_speedup"] = d["1"]["wall_ms"] / d["16"]["wall_ms"]
        if f"scal_q{q}_t16_scan_lineitem_ms" in NUM:
            NUM[f"scal_q{q}_scan_cpu_ratio"] = NUM[f"scal_q{q}_t16_scan_lineitem_ms"] / NUM[f"scal_q{q}_t1_scan_lineitem_ms"]
    if rows:
        put_table("scaling_probe", ["Query", "Threads", "Wall (ms)", "Process CPU (ms)", "CPU inside operators (ms)",
                                    "CPU outside operators"], rows)


def headline_table(res):
    """One row per engine: the numbers of the summary (all ratios to DuckDB, geometric means over queries)."""
    cols = [("sf1_t1", "TPC-H SF1, 1 thread"), ("sf1_t16", "SF1, 16 threads"), ("sf0.1_t16", "SF0.1, 16 threads"),
            ("micro_t1", "operators, 1 thread"), ("h2o-g1_t1", "H2O groupby, 1 thread"), ("h2o-j1_t1", "H2O join, 1 thread")]
    rows = []
    for e in ("cdb", "duckdb", "datafusion", "chdb", "polars", "sqlite"):
        row = [A.NAMES[e]]
        for key, _ in cols:
            if e == "duckdb":
                row.append("1")
                continue
            v = NUM.get(f"{key}_ratio_{e}") if key.startswith("sf") else NUM.get(f"{key}_ratio_{e}")
            row.append(f"{v:.2f}" if v is not None else "–")
        rss = NUM.get(f"rss_{e}_t1")
        row.append(f"{rss:,.0f}" if rss else "–")
        rows.append(row)
    put_table("headline", ["Engine"] + [c[1] for c in cols] + ["Peak RSS, SF1, 1 thread (MB)"], rows)


def noise_chart(plt, res, out):
    files = sorted(glob.glob(os.path.join(res, "noise_*.jsonl")))
    if not files:
        return
    import collections
    data = collections.defaultdict(list)
    for p in files:
        stage = os.path.basename(p)[len("noise_"):-6]
        for line in open(p):
            r = json.loads(line)
            if r["query"] == "q06" and r["threads"] == 1:
                data[(r["engine"], stage)].append(r["wall_ms"])
    engines = [e for e in ("cdb", "duckdb", "datafusion", "chdb", "polars") if any(k[0] == e for k in data)]
    fig, ax = plt.subplots(figsize=(7.0, 3.4))
    for i, e in enumerate(engines):
        allm = common.median([x for (en, _), v in data.items() if en == e for x in v])
        for j, stage in enumerate(("start", "mid", "end")):
            v = data.get((e, stage))
            if not v:
                continue
            xs = [i + (j - 1) * 0.26 + ((k * 0.6180339) % 1 - 0.5) * 0.18 for k in range(len(v))]
            ax.scatter(xs, [x / allm for x in v], s=7, color=A.COLORS[e], alpha=0.55 + 0.2 * j, linewidths=0)
    ax.axhline(1.0, color="#52514e", lw=1)
    ax.set_xticks(range(len(engines)))
    ax.set_xticklabels([A.NAMES[e].replace(" (chDB)", "") for e in engines])
    ax.set_ylabel("single run / median of the engine's runs")
    ax.set_title("Run-to-run noise: TPC-H Q6 at SF1, one thread, 50 repeats at the start, middle, end (left to right)", loc="left", fontsize=9)
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    save(plt, fig, os.path.join(out, "noise.svg"))


def memory_chart(plt, stages, out):
    st = stages.get((1, 1))
    if not st:
        return
    engines = [e for e in st.engines if e in ("cdb", "duckdb", "datafusion", "chdb", "polars", "sqlite")]
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(7.4, 3.2))
    for ax, key, ylabel in ((a1, "load_wall_s", "load time, 8 tables (s)"), (a2, "hwm_kb", "peak resident memory (MB)")):
        for i, e in enumerate(engines):
            v = common.median(st.visit_values(e, key))
            v = v / 1024 if key == "hwm_kb" else v
            ax.bar(i, v, width=0.6, color=A.COLORS[e], zorder=2)
            ax.text(i, v * 1.03, f"{v:,.0f}" if v >= 100 else f"{v:.1f}", ha="center", va="bottom", fontsize=8)
        ax.set_xticks(range(len(engines)))
        ax.set_xticklabels([A.NAMES[e].replace(" (chDB)", "") for e in engines], rotation=30, ha="right")
        ax.set_ylabel(ylabel)
        ax.grid(axis="x", visible=False)
    fig.suptitle("TPC-H SF1, one thread: loading from CSV and memory footprint", x=0.01, ha="left", fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    save(plt, fig, os.path.join(out, "load_memory_sf1.svg"))


# ---------------------------------------------------------------------------------- main

def run(args):
    res, out = args.results, args.out
    os.makedirs(os.path.join(out, "tables"), exist_ok=True)
    plt = setup_matplotlib()

    noise_files = sorted(glob.glob(os.path.join(res, "noise_*.jsonl")))
    rows, tau = A.noise_summary(noise_files) if noise_files else ([], {1: 0.05, 16: 0.1})
    NUM["tau1"], NUM["tau16"] = tau[1], tau[16]
    for t in (1, 16):
        sp = [r["spread"] for r in rows if r["threads"] == t]
        if sp:
            num(f"noise_spread{t}_min", min(sp) * 100, f"{min(sp) * 100:.0f}")
            num(f"noise_spread{t}_max", max(sp) * 100, f"{max(sp) * 100:.0f}")
    if rows:
        num("noise_ghz_min", min(r["freq_avg_mhz"] for r in rows) / 1000, f"{min(r['freq_avg_mhz'] for r in rows) / 1000:.1f}")
        num("noise_ghz_max", max(r["freq_avg_mhz"] for r in rows) / 1000, f"{max(r['freq_avg_mhz'] for r in rows) / 1000:.1f}")
    if rows:
        trows = []
        for r in sorted(rows, key=lambda r: (r["threads"], r["engine"], r["query"], r["stage"])):
            trows.append([A.NAMES[r["engine"]], r["query"].upper(), r["threads"], r["stage"], r["n"],
                          fmt_ms(r["median_ms"]), f"{100 * r['cv']:.1f}%", f"{100 * r['spread']:.1f}%",
                          f"{r['max_over_median']:.2f}", f"{r['freq_avg_mhz'] / 1000:.2f}"])
        put_table("noise", ["Engine", "Query", "Threads", "When", "Runs", "Median (ms)", "CV", "p10–p90 spread",
                            "Max / median", "Mean CPU GHz"], trows, ["l", "l", "r", "l", "r", "r", "r", "r", "r", "r"])

    stages = {}
    for sf in (0.1, 1, 3):
        for t in (1, 2, 4, 8, 16):
            st = A.Stage(os.path.join(res, f"tpch_sf{sf:g}_t{t}.jsonl"))
            if st.visits:
                stages[(sf, t)] = st
    for (sf, t), st in stages.items():
        bad = correctness(res, sf, t)
        tau_t = stage_tau(st)
        key = f"sf{sf:g}_t{t}"
        num(f"tau_{key}_pct", tau_t * 100, f"{tau_t * 100:.0f}")
        st.tau = tau_t
        TAUS.append([f"TPC-H SF{sf:g}", t, f"{tau_t * 100:.0f}%"])
        qs = tpch_stage_tables(st, f"tpch_sf{sf:g}_t{t}", "duckdb", tau_t, bad)
        NUM[f"{key}_queries"] = len(qs)
        for e in st.engines:
            NUM[f"{key}_gm_{e}"] = st.geomean(e, qs)[0]
            if e != "duckdb" and "duckdb" in st.engines:
                med, lo, hi = st.ratio_geomean(e, "duckdb", qs)
                NUM[f"{key}_ratio_{e}"], NUM[f"{key}_ratio_{e}_lo"], NUM[f"{key}_ratio_{e}_hi"] = med, lo, hi
        NUM[f"{key}_flagged_visits"] = st.flagged_visits()
        NUM[f"{key}_retried_attempts"] = st.retried_attempts()
        NUM[f"{key}_rounds"] = len(st.rounds)
        if (sf, t) in ((1, 1), (1, 16), (0.1, 1), (0.1, 16)):
            chart_dot_ratio(plt, os.path.join(out, f"tpch_sf{sf:g}_t{t}_ratio.svg"), st, "duckdb", st.queries,
                            f"TPC-H SF{sf:g}, {t} thread{'s' if t > 1 else ''}: time per query relative to DuckDB", tau_t)

    for sf in (0.1, 1, 3):
        panels = [(f"{t} thread{'s' if t > 1 else ''}", stages[(sf, t)],
                   stages[(sf, t)].common_queries([e for e in stages[(sf, t)].engines if e != "sqlite"]))
                  for t in (1, 16) if (sf, t) in stages]
        if panels:
            chart_geomean(plt, os.path.join(out, f"tpch_sf{sf:g}_geomean.svg"), panels, "duckdb",
                          f"TPC-H SF{sf:g}: geometric-mean time relative to DuckDB")

    by_t = {t: stages[(1, t)] for t in (1, 2, 4, 8, 16) if (1, t) in stages}
    if len(by_t) >= 3:
        chart_scaling(plt, os.path.join(out, "scaling_sf1.svg"), by_t, ["cdb", "duckdb", "datafusion", "chdb", "polars"],
                      "TPC-H SF1: scaling with threads")
        rows = []
        for e in ["cdb", "duckdb", "datafusion", "chdb", "polars"]:
            row = [A.NAMES[e]]
            base = None
            for t in sorted(by_t):
                st = by_t[t]
                if e not in st.engines:
                    row.append("–")
                    continue
                qs = st.common_queries([e])
                g = st.geomean(e, qs)[0]
                base = base or g
                row.append(f"{fmt_ms(g)} ({base / g:.1f}x)")
                NUM[f"speedup{t}_{e}"] = base / g
            rows.append(row)
        put_table("scaling_sf1", ["Engine"] + [f"{t} thr" for t in sorted(by_t)], rows)

    codebase_numbers()
    for t in (1, 16):
        r = NUM.get(f"sf1_t{t}_ratio_sqlite")
        c = NUM.get(f"sf1_t{t}_ratio_cdb")
        if r and c:
            num(f"sf1_t{t}_sqlite_over_cdb", r / c, f"{r / c:.1f}")
    num("results_date", os.path.basename(res.rstrip("/")))
    environment(res)
    num("single_run_spread1_pct", tau[1] * 100, f"{tau[1] * 100:.0f}")
    num("single_run_spread16_pct", tau[16] * 100, f"{tau[16] * 100:.0f}")
    num("rounds_sf1", max([len(stages[(1, t)].rounds) for t in (1, 16) if (1, t) in stages] or [0]))
    num("sf3_note", "and 3 (only if memory allowed)" if (3, 1) in stages else "(SF3 was not run: the desktop did not leave enough free memory)")
    standing_tables(stages, tau)
    load_memory_table(res, stages)
    efficiency_table(stages)
    sf_scaling_table(stages)
    cold_warm_table(stages)
    correctness_table(res)
    variants_table(res, stages)
    for name in ("micro", "h2o-g1", "h2o-j1"):
        workload_stage(res, name, plt, out, tau)
    profile_section(res)
    scaling_probe_section(res)
    bandwidth_table(res, {t: A.Stage(os.path.join(res, f"micro_t{t}.jsonl")) for t in (1, 16)})
    optimizer_section(res, plt, out)
    durability_section(res, plt, out)
    headline_table(res)
    noise_chart(plt, res, out)
    memory_chart(plt, stages, out)
    put_table("tau", ["Configuration", "Threads", "Tie band τ"], sorted(TAUS, key=lambda r: (r[0], r[1])), ["l", "r", "r"])
    abl = os.path.join(res, "ablation_sf0.01.md")
    if os.path.exists(abl):
        TABLES["ablation"] = open(abl).read()
        num("ablation_dnf", TABLES["ablation"].count("> 45 s"))
    def default_text(k, v):
        if k.startswith("top") and "_pct_" in k or k.startswith("scal_") and k.endswith("_outside"):
            return f"{v:.0f}"
        if k.startswith(("ir_ratio_", "dlmr_ratio_", "r_cout_", "scal_")) and isinstance(v, float):
            return f"{v:.1f}" if v >= 0.1 else f"{v:.2f}"
        if k.startswith(("ms_", "cpu_s_", "load_", "rss_")):
            return fmt_ms(v) if k.startswith("ms_") else (f"{v:,.0f}" if k.startswith("rss_") else f"{v:.1f}")
        if isinstance(v, int):
            return f"{v:,}"
        return f"{v:.2f}" if isinstance(v, float) else str(v)
    json.dump({"raw": NUM, "text": {**{k: default_text(k, v) for k, v in NUM.items()}, **TEXT}},
              open(os.path.join(out, "numbers.json"), "w"), indent=1, sort_keys=True)
    for name, text in TABLES.items():
        open(os.path.join(out, "tables", f"{name}.md"), "w").write(text)
    print(f"{len(TABLES)} tables, {len(NUM)} numbers")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--out", default=os.path.join(common.ROOT, "docs", "report"))
    run(ap.parse_args())
