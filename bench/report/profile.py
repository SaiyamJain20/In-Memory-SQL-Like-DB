#!/usr/bin/env python3
"""Instruction-level profile of one query in one engine, with a simulated cache (callgrind).

    profile.py --engine cdb --query 9 --sf 0.1 --out results/profile_cdb_q09.json

`perf` and the hardware counters are not available on the test machine (no root), so this is the
only look inside a query: valgrind --tool=callgrind --cache-sim=yes, instrumentation switched on
only around ONE warm execution of the query (callgrind_control -i on / off), so loading and the
warm-up are not counted. The result is the number of instructions, the simulated last-level
misses of data reads, and the functions that executed the most instructions.

The simulated numbers are exact counts of a machine model, not measurements of this CPU: use them to
compare engines and to find where the instructions go, not to predict wall-clock time.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
import driver  # noqa: E402
import workloads  # noqa: E402


def callgrind_totals(path):
    """(events, totals, top functions) of a callgrind output file. Totals are over every event of the
    cache simulation; the function list is by instructions (Ir) with the simulated last-level data
    read misses (DLmr) next to it."""
    out = subprocess.run(["callgrind_annotate", "--auto=no", path], capture_output=True, text=True).stdout
    events, totals = None, None
    for line in out.splitlines():
        if line.startswith("Events shown:"):
            events = line.split(":", 1)[1].split()
        if "PROGRAM TOTALS" in line:
            totals = [int(x.replace(",", "")) for x in re.findall(r"([\d,]+)\s+\(", line)]
    out = subprocess.run(["callgrind_annotate", "--auto=no", "--show=Ir,DLmr", "--sort=Ir", path],
                         capture_output=True, text=True).stdout
    funcs = []
    for line in out.splitlines():
        m = re.match(r"^\s*([\d,]+)\s+\(\s*([\d.]+)%\)\s+([\d,]+|\.)\s+(?:\(\s*([\d.]+)%\)\s+)?(.*)$", line)
        if m and "PROGRAM TOTALS" not in line and "file:function" not in line:
            funcs.append({"ir": int(m.group(1).replace(",", "")), "pct": float(m.group(2)),
                          "dlmr": 0 if m.group(3) == "." else int(m.group(3).replace(",", "")),
                          "name": m.group(5).strip()[:150]})
    return events, totals, funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="cdb")
    ap.add_argument("--query", type=int, default=1, help="TPC-H query number")
    ap.add_argument("--micro", default=None, help="a micro-benchmark query name instead (data/micro_1m)")
    ap.add_argument("--sf", type=float, default=0.1)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    if args.micro:
        wl = workloads.micro("micro_1m")
        name, spec, src = args.micro, wl.spec, wl.file(args.engine, args.micro)
    else:
        name = f"q{args.query:02d}"
        spec = common.tpch_spec(args.sf)
        src = common.query_file(args.engine, name)
        if args.engine == "polars":
            src = os.path.join(common.REPORT_DIR, "queries", "polars", f"{name}.py")
    with tempfile.TemporaryDirectory() as tmp:
        cg = os.path.join(tmp, "callgrind.out")
        prefix = ["valgrind", "--tool=callgrind", "--instr-atstart=no", "--cache-sim=yes", f"--callgrind-out-file={cg}"]
        w = driver.Worker(args.engine, 1, args.sf, prefix=prefix)
        try:
            driver.load_tables(w, spec, tmp)
            w.call("RUN", src, 2, timeout=7200)  # warm up, not instrumented
            subprocess.run(["callgrind_control", "-i", "on", str(w.p.pid)], capture_output=True)
            w.call("RUN", src, 1, timeout=7200)
            subprocess.run(["callgrind_control", "-i", "off", str(w.p.pid)], capture_output=True)
        finally:
            w.close()
        events, totals, funcs = callgrind_totals(cg)
    result = {"engine": args.engine, "query": name, "sf": args.sf, "events": events, "totals": totals,
              "top_functions": funcs[:25]}
    json.dump(result, open(args.out, "w"), indent=1)
    print(args.engine, name, events, totals)


if __name__ == "__main__":
    main()
