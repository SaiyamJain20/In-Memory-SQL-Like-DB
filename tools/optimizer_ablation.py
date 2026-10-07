#!/usr/bin/env python3
"""The optimizer on and off, query by query: cdb_tpch --no-optimizer runs the plan exactly as the binder
built it (comma joins are cross products with the predicates on top, subqueries are whatever they
unnested into), so the interesting queries do not finish. Each run has a wall-clock limit.

    .venv/bin/python tools/optimizer_ablation.py --sf 0.01 --limit 60

Prints a markdown table: the minimum of 3 optimized runs, the unoptimized time (one run) or "> limit".
"""
import argparse
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(binary, sf, q, runs, extra, limit):
    cmd = [binary, "--sf", str(sf), "--threads", "1", "--runs", str(runs), "--queries", str(q)] + extra
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=limit).stdout
    except subprocess.TimeoutExpired:
        return None
    for line in out.splitlines():
        m = re.match(r"^\s*(\d+)\s+(\d+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)", line)
        if m and int(m.group(1)) == q:
            return float(m.group(4))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sf", type=float, default=0.01)
    ap.add_argument("--limit", type=int, default=60, help="seconds per unoptimized run")
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "release", "bench", "cdb_tpch"))
    args = ap.parse_args()
    print(f"| Query | optimizer on (ms) | optimizer off (ms) |")
    print("|---|---:|---:|")
    for q in range(1, 23):
        on = run(args.binary, args.sf, q, 3, [], 600)
        off = run(args.binary, args.sf, q, 1, ["--no-optimizer"], args.limit)
        print(f"| Q{q} | {on:.1f} | " + (f"{off:.1f}" if off is not None else f"> {args.limit} s") + " |", flush=True)


if __name__ == "__main__":
    main()
