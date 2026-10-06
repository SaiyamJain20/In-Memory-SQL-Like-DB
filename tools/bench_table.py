#!/usr/bin/env python3
"""Builds the TPC-H comparison tables of docs/BENCHMARKS.md from saved cdb_tpch / tpch_duckdb_time.py output.

    for t in 1 16; do build/release/bench/cdb_tpch --sf 1 --threads $t --runs 5 > out/cdb_t$t.txt; done
    for t in 1 16; do .venv/bin/python tools/tpch_duckdb_time.py --sf 1 --threads $t --runs 5 > out/duck_t$t.txt; done
    .venv/bin/python tools/bench_table.py out/cdb_t1.txt out/cdb_t16.txt out/duck_t1.txt out/duck_t16.txt
    # with a second build of cdb_tpch (e.g. before a change) as the 5th and 6th arguments:
    .venv/bin/python tools/bench_table.py a1 a16 d1 d16 before_t1.txt before_t16.txt

Each file's rows look like `q  rows  cold ms  min ms  median ms`; the minimum is used.
"""
import math
import re
import sys


def read(path):
    out = {}
    for line in open(path):
        m = re.match(r"^\s*(\d+)\s+(\d+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)", line)
        if m:
            out[int(m.group(1))] = float(m.group(4))
    return out


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def main():
    files = sys.argv[1:]
    if len(files) not in (4, 6):
        sys.exit(__doc__)
    c1, c16, d1, d16 = (read(f) for f in files[:4])
    before = (read(files[4]), read(files[5])) if len(files) == 6 else None
    qs = sorted(c1)
    head = "| Query | cdb 1 thr | cdb 16 thr | speedup | DuckDB 1 thr | DuckDB 16 thr | cdb / DuckDB @1 | cdb / DuckDB @16 |"
    sep = "|---|---:|---:|---:|---:|---:|---:|---:|"
    if before:
        head += " before 1 thr | before 16 thr | 1 thr: before / now |"
        sep += "---:|---:|---:|"
    print(head)
    print(sep)
    cols = {"sp": [], "r1": [], "r16": [], "ba": []}
    for q in qs:
        row = [f"Q{q}", f"{c1[q]:.1f}", f"{c16[q]:.1f}", f"{c1[q] / c16[q]:.1f}x", f"{d1[q]:.1f}", f"{d16[q]:.1f}",
               f"{c1[q] / d1[q]:.2f}x", f"{c16[q] / d16[q]:.2f}x"]
        cols["sp"].append(c1[q] / c16[q])
        cols["r1"].append(c1[q] / d1[q])
        cols["r16"].append(c16[q] / d16[q])
        if before:
            b1, b16 = before[0][q], before[1][q]
            row += [f"{b1:.1f}", f"{b16:.1f}", f"{b1 / c1[q]:.2f}x"]
            cols["ba"].append(b1 / c1[q])
        print("| " + " | ".join(row) + " |")
    row = ["**geometric mean**", "", "", f"**{geomean(cols['sp']):.1f}x**", "", "",
           f"**{geomean(cols['r1']):.2f}x**", f"**{geomean(cols['r16']):.2f}x**"]
    if before:
        row += ["", "", f"**{geomean(cols['ba']):.2f}x**"]
    print("| " + " | ".join(row) + " |")
    # the twelve queries of the earlier tables
    old = [1, 3, 5, 6, 7, 8, 9, 10, 12, 13, 14, 19]
    print(f"\nthe 12 queries of the earlier tables: speedup {geomean([c1[q] / c16[q] for q in old]):.2f}x, "
          f"cdb / DuckDB at 1 thread {geomean([c1[q] / d1[q] for q in old]):.2f}x, "
          f"at 16 threads {geomean([c16[q] / d16[q] for q in old]):.2f}x")


if __name__ == "__main__":
    main()
