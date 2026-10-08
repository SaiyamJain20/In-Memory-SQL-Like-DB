#!/usr/bin/env python3
"""Cross-engine benchmark driver (docs/REPORT.md).

    driver.py verify --workload tpch --sf 0.01 [--engines cdb,duckdb,...] [--threads 4]
    driver.py check  --workload micro|h2o-g1|h2o-j1 [--engines ...]        # answers vs DuckDB's
    driver.py run    --workload tpch --sf 1 --threads 1 --rounds 5 --out results/x.jsonl
    driver.py noise  --engine duckdb --query 6 --sf 1 --repeats 50 --out results/noise.jsonl

Every engine runs in its own worker process (cdb_report_worker or py_worker.py) behind the same
line protocol. A *visit* is one fresh worker: load the data, run each query once cold and then a
few times warm, report. Rounds visit the engines in a rotating order, so that whatever the
machine was doing (it is a desktop in normal use) hits every engine alike; each visit is
bracketed by control probes and a measurement of the CPU other processes used meanwhile, and a
visit that is not comparable with the others is repeated (all attempts are kept in the file).
"""
import argparse
import json
import os
import select
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
import workloads  # noqa: E402

PY = os.path.join(common.ROOT, ".venv-bench", "bin", "python")
CDB_WORKER = os.path.join(common.ROOT, "build", "release", "bench", "cdb_report_worker")
ALL_ENGINES = ["cdb", "duckdb", "datafusion", "chdb", "polars", "sqlite"]
CLK = os.sysconf("SC_CLK_TCK")
# rough resident footprint in MB per unit of data (SF for TPC-H, 10M rows for the others), to
# refuse a visit that would push the desktop into swap
FOOTPRINT_MB = {"cdb": 1500, "duckdb": 1600, "duckdb-decimal": 1600, "datafusion": 3500, "chdb": 3000,
                "chdb-mergetree": 3000, "polars": 3500, "sqlite": 2500}


class WorkerError(Exception):
    pass


class Worker:
    def __init__(self, engine, threads, sf=1.0, prefix=()):
        self.engine, self.threads = engine, threads
        env = dict(os.environ, TPCH_SF=f"{sf:g}", OMP_NUM_THREADS="1", POLARS_MAX_THREADS=str(threads),
                   TOKIO_WORKER_THREADS=str(threads), RAYON_NUM_THREADS=str(threads),
                   OPENBLAS_NUM_THREADS="1")
        cmd = [CDB_WORKER] if engine == "cdb" else [PY, os.path.join(common.REPORT_DIR, "py_worker.py"),
                                                    "--engine", engine]
        cmd = list(prefix) + cmd  # e.g. valgrind --tool=callgrind (bench/report/profile.py)
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1, env=env)
        self.call("THREADS", threads, timeout=1800 if prefix else 120)

    def call(self, *fields, timeout=600):
        self.p.stdin.write("\t".join(str(f) for f in fields) + "\n")
        self.p.stdin.flush()
        ready, _, _ = select.select([self.p.stdout], [], [], timeout)
        if not ready:
            self.kill()
            raise TimeoutError(f"{self.engine}: no reply to {fields[0]} in {timeout} s")
        line = self.p.stdout.readline()
        if not line:
            raise WorkerError(f"{self.engine}: worker died during {fields[0]}")
        reply = json.loads(line)
        if not reply.get("ok"):
            raise WorkerError(f"{self.engine}: {reply.get('error')}")
        return reply

    def hwm_kb(self):
        try:
            for line in open(f"/proc/{self.p.pid}/status"):
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
        except OSError:
            pass
        return -1

    def cpu_jiffies(self):
        return common.read_proc_cpu_jiffies(self.p.pid)

    def kill(self):
        try:
            self.p.kill()
            self.p.wait(timeout=10)
        except Exception:
            pass

    def close(self):
        try:
            self.call("QUIT", timeout=30)
            self.p.wait(timeout=30)
        except Exception:
            self.kill()


# ---------------------------------------------------------------------------------- loading

CDB_TYPES = {"i32": "INTEGER", "i64": "BIGINT", "f64": "DOUBLE", "date": "DATE", "str": "VARCHAR"}


def load_tables(worker, spec, tmp):
    """Creates and fills the tables of `spec`; returns {table: {ms, rows}}."""
    if worker.engine != "cdb":
        path = os.path.join(tmp, "spec.json")
        json.dump(spec, open(path, "w"))
        return worker.call("LOADSPEC", path, timeout=7200)["tables"]
    out = {}
    for t in spec["tables"]:
        cols = ", ".join(f"{c} {CDB_TYPES[ty]} NOT NULL" for c, ty in t["columns"])
        path = os.path.join(tmp, f"load_{t['name']}.sql")
        header = "TRUE" if t["header"] else "FALSE"
        open(path, "w").write(
            f"CREATE TABLE {t['name']} ({cols});\n"
            f"COPY {t['name']} FROM '{t['path']}' (DELIMITER '{t['delimiter']}', HEADER {header});\n")
        r = worker.call("EXEC", path, timeout=7200)
        out[t["name"]] = {"ms": r["ms"], "rows": None}
    return out


def need_mb(engine, scale):
    return int(FOOTPRINT_MB.get(engine, 3000) * max(scale, 0.05) * 1.3 + 300)


def wait_for_memory(engine, scale, max_wait=900):
    need = need_mb(engine, scale)
    waited = 0
    while common.read_meminfo_available_mb() < need:
        if waited >= max_wait:
            return False
        time.sleep(15)
        waited += 15
    return True


def scale_of(args):
    """Memory scale of a workload: the scale factor for TPC-H, 1 (10M rows) for the others."""
    return args.sf if args.workload == "tpch" else 1.0


def make_workload(args):
    if args.workload == "tpch":
        return workloads.tpch(args.sf, [int(q) for q in args.queries.split(",")])
    return workloads.get(args.workload)


# ---------------------------------------------------------------------------------- verify / check

def dump_and_compare(w, wl, engine, q, want_path, tmp, timeout, rel):
    src = wl.file(engine, q)
    if src is None:
        return "n/a", "the engine has no such function"
    out = os.path.join(tmp, f"{q}.csv")
    w.call("DUMP", src, out, timeout=timeout)
    return common.compare_results(out, want_path, rel=rel)


def cmd_verify(args):
    """Every engine's answer to every query against the answer files of the workload: DuckDB's own
    for TPC-H (data/tpch-sf*/expected), DuckDB's fresh answers for the other workloads."""
    wl = make_workload(args)
    scale = scale_of(args)
    results = {}
    expected_dir = os.path.join(common.ROOT, "data", f"tpch-sf{args.sf:g}", "expected")
    engines = args.engines.split(",")
    with tempfile.TemporaryDirectory() as reftmp:
        if args.workload != "tpch":
            # the reference answers: DuckDB, 4 threads, on the same data
            ref = Worker("duckdb", 4, 1.0)
            load_tables(ref, wl.spec, reftmp)
            for q in wl.queries:
                ref.call("DUMP", wl.file("duckdb", q), os.path.join(reftmp, f"ref_{q}.csv"), timeout=3600)
            ref.close()
        for engine in engines:
            results[engine] = {}
            if not wait_for_memory(engine, scale):
                results[engine] = {q: {"status": "skipped", "detail": "memory"} for q in wl.queries}
                print(f"{engine}: skipped, not enough free memory", flush=True)
                continue
            with tempfile.TemporaryDirectory() as tmp:
                w = Worker(engine, args.threads, args.sf)
                try:
                    load_tables(w, wl.spec, tmp)
                    for q in wl.queries:
                        want = (os.path.join(expected_dir, f"{q}.csv") if args.workload == "tpch"
                                else os.path.join(reftmp, f"ref_{q}.csv"))
                        try:
                            status, detail = dump_and_compare(w, wl, engine, q, want, tmp, args.timeout, args.rel)
                        except TimeoutError:
                            status, detail = "timeout", f"> {args.timeout} s"
                            w = Worker(engine, args.threads, args.sf)
                            load_tables(w, wl.spec, tmp)
                        except WorkerError as e:
                            status, detail = "error", str(e)[:300]
                        results[engine][q] = {"status": status, "detail": detail}
                        print(f"{engine:16s} {q} {status} {detail}", flush=True)
                finally:
                    w.close()
    if args.out:
        json.dump(results, open(args.out, "w"), indent=1)
    bad = [(e, q) for e, r in results.items() for q, v in r.items()
           if not (v["status"].startswith("ok") or v["status"] == "n/a")]
    print(f"\n{sum(len(r) for r in results.values()) - len(bad)} ok, {len(bad)} not ok")
    return 1 if bad else 0


# ---------------------------------------------------------------------------------- visits

def timeout_for(engine, args):
    """Seconds one query may take: --timeout, or an engine's own limit from --engine-timeouts."""
    for item in (args.engine_timeouts or "").split(","):
        if item.startswith(engine + "="):
            return int(item.split("=")[1])
    return args.timeout


def run_visit(engine, wl, threads, rnd, order_pos, args, known_dnf):
    timeout = timeout_for(engine, args)
    sf = args.sf
    key = (engine, wl.name)
    rec = {"engine": engine, "workload": wl.name, "sf": sf, "threads": threads, "round": rnd,
           "order_pos": order_pos, "start": time.time(), "queries": {}}
    if not wait_for_memory(engine, scale_of(args)):
        rec["skipped"] = f"MemAvailable stayed below {need_mb(engine, scale_of(args))} MB"
        return rec
    with tempfile.TemporaryDirectory() as tmp:
        rec["probe_before"] = common.control_probe()
        busy0, _ = common.read_proc_stat_busy()
        t0 = time.perf_counter()
        w = Worker(engine, threads, sf)
        worker_jiffies = 0
        try:
            t = time.perf_counter()
            rec["load"] = load_tables(w, wl.spec, tmp)
            rec["load_wall_s"] = time.perf_counter() - t
            rec["hwm_after_load_kb"] = w.hwm_kb()
            rec["cpu_jiffies_after_load"] = w.cpu_jiffies()
            for q in wl.queries:
                src = wl.file(engine, q)
                if src is None:
                    rec["queries"][q] = {"na": True}
                    continue
                if q in known_dnf.get(key, set()):
                    rec["queries"][q] = {"dnf": True, "skipped_known": True}
                    continue
                entry = {}
                try:
                    cold = w.call("RUN", src, 1, timeout=timeout)
                    entry.update(rows=cold["rows"], cold_ms=cold["wall_ms"][0], cold_cpu_ms=cold["cpu_ms"][0])
                    extra = args.runs - 1 if cold["wall_ms"][0] < args.slow_ms else min(1, args.runs - 1)
                    if extra > 0:
                        warm = w.call("RUN", src, extra, timeout=timeout)
                        entry.update(warm_ms=warm["wall_ms"], warm_cpu_ms=warm["cpu_ms"])
                except TimeoutError:
                    entry.update(dnf=True, timeout_s=timeout)
                    known_dnf.setdefault(key, set()).add(q)
                    w = Worker(engine, threads, sf)  # the killed worker lost its data
                    load_tables(w, wl.spec, tmp)
                except WorkerError as e:
                    entry.update(error=str(e)[:300])
                rec["queries"][q] = entry
            rec["hwm_kb"] = w.hwm_kb()
            worker_jiffies = w.cpu_jiffies()
            rec["os_threads"] = w.call("STAT").get("os_threads")
            if engine == "cdb":
                rec["stored_bytes"] = w.call("STORED", ",".join(t["name"] for t in wl.spec["tables"]))["bytes"]
        finally:
            w.close()
        wall = time.perf_counter() - t0
        busy1, _ = common.read_proc_stat_busy()
        rec["probe_after"] = common.control_probe()
        rec["visit_wall_s"] = wall
        # CPU the rest of the machine used while this visit ran, in cores
        rec["background_cores"] = max(0.0, ((busy1 - busy0) - worker_jiffies) / CLK / wall)
    return rec


def visit_flags(rec, probes, args):
    """Why a visit is not comparable with the others: the machine was slower than the baseline
    (control probes > 15% above the median of the earlier visits) or the rest of the machine was
    busy during it (more than --max-bg-cores of CPU used by other processes)."""
    flags = []
    if "probe_before" not in rec:
        return flags
    if len(probes) >= 6:
        base = common.median(probes)
        for k in ("probe_before", "probe_after"):
            if rec[k]["cpu_s"] > base * args.probe_tolerance:
                flags.append(f"{k}: {rec[k]['cpu_s'] / base:.2f}x the baseline")
    if rec.get("background_cores", 0) > args.max_bg_cores:
        flags.append(f"background {rec['background_cores']:.1f} cores")
    return flags


def cmd_run(args):
    wl = make_workload(args)
    engines = args.engines.split(",")
    known_dnf = {}
    if args.known_dnf and os.path.exists(args.known_dnf):
        for k, v in json.load(open(args.known_dnf)).items():
            e, name = k.split("|")
            known_dnf[(e, name)] = set(v)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "a") as out:
        probes = []  # control probe seconds of every visit so far: the baseline of "quiet"
        for rnd in range(args.rounds):
            order = engines[rnd % len(engines):] + engines[:rnd % len(engines)]
            for pos, engine in enumerate(order):
                th = 1 if engine == "sqlite" else args.threads
                for attempt in range(1 + args.retries):
                    rec = run_visit(engine, wl, th, rnd, pos, args, known_dnf)
                    rec["attempt"] = attempt
                    flagged = visit_flags(rec, probes, args)
                    rec["flags"] = flagged
                    out.write(json.dumps(rec) + "\n")
                    out.flush()
                    if "probe_before" in rec:
                        probes += [rec["probe_before"]["cpu_s"], rec["probe_after"]["cpu_s"]]
                    done = [q for q, v in rec["queries"].items() if "cold_ms" in v]
                    print(f"{wl.name} round {rnd} {engine:14s} threads={th} try={attempt} "
                          f"ok={len(done)}/{len(rec['queries'])} load={rec.get('load_wall_s', 0):.1f}s "
                          f"bg={rec.get('background_cores', 0):.2f} cores flags={flagged} {rec.get('skipped', '')}",
                          flush=True)
                    if not flagged:
                        break
    if args.known_dnf:
        json.dump({f"{e}|{n}": sorted(v) for (e, n), v in known_dnf.items()}, open(args.known_dnf, "w"))


# ---------------------------------------------------------------------------------- noise floor

def cmd_noise(args):
    wl = make_workload(args)
    q = f"q{args.query:02d}" if args.workload == "tpch" else args.query_name
    with tempfile.TemporaryDirectory() as tmp, open(args.out, "a") as out:
        w = Worker(args.engine, args.threads, args.sf)
        try:
            load_tables(w, wl.spec, tmp)
            src = wl.file(args.engine, q)
            w.call("RUN", src, 2, timeout=args.timeout)  # warm up
            for i in range(args.repeats):
                probe = common.control_probe(cpu_iters=500_000, mem_mb=64)
                r = w.call("RUN", src, 1, timeout=args.timeout)
                out.write(json.dumps({"engine": args.engine, "query": q, "workload": wl.name,
                                      "threads": args.threads, "i": i, "wall_ms": r["wall_ms"][0],
                                      "cpu_ms": r["cpu_ms"][0], "probe": probe, "t": time.time()}) + "\n")
                out.flush()
        finally:
            w.close()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("verify", "check", "run", "noise"):
        p = sub.add_parser(name)
        p.add_argument("--workload", default="tpch", choices=["tpch", "micro", "h2o-g1", "h2o-j1"])
        p.add_argument("--sf", type=float, default=0.01)
        p.add_argument("--threads", type=int, default=1)
        p.add_argument("--engines", default=",".join(ALL_ENGINES))
        p.add_argument("--queries", default=",".join(str(i) for i in range(1, 23)))
        p.add_argument("--timeout", type=int, default=300)
        p.add_argument("--out", default=None)
        if name in ("verify", "check"):
            p.add_argument("--rel", type=float, default=1e-7)
        if name == "run":
            p.add_argument("--rounds", type=int, default=5)
            p.add_argument("--runs", type=int, default=4, help="cold run + (runs-1) warm runs")
            p.add_argument("--slow-ms", type=float, default=10000, help="above this a query is run once more only")
            p.add_argument("--known-dnf", default=None)
            p.add_argument("--engine-timeouts", default=None, help="e.g. sqlite=60")
            p.add_argument("--retries", type=int, default=2, help="re-runs of a flagged visit")
            p.add_argument("--probe-tolerance", type=float, default=1.15)
            p.add_argument("--max-bg-cores", type=float, default=2.5)
        if name == "noise":
            p.add_argument("--engine", default="duckdb")
            p.add_argument("--query", type=int, default=6)
            p.add_argument("--query-name", default="scan_sum")
            p.add_argument("--repeats", type=int, default=50)
    args = ap.parse_args()
    if args.cmd in ("verify", "check"):
        sys.exit(cmd_verify(args))
    if args.cmd == "run":
        cmd_run(args)
    if args.cmd == "noise":
        cmd_noise(args)


if __name__ == "__main__":
    main()
