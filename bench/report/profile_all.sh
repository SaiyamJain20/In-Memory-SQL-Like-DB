#!/bin/sh
# Instruction-level profiles (callgrind, simulated cache) of the queries chapter 7 explains, for cdb and
# DuckDB, on 1,000,000-row micro tables and TPC-H SF0.1. Deterministic: independent of machine noise.
#   bench/report/profile_all.sh <results dir>
OUT=${1:?results dir}/profiles
PY=.venv-bench/bin/python
mkdir -p "$OUT"
for e in cdb duckdb; do
  for q in topn_10 sort_5m join_1k join_1m agg_1m agg_100k scan_count scan_sum filter_90pct; do
    [ -e "$OUT/${e}_$q.json" ] || $PY bench/report/profile.py --engine $e --micro $q --out "$OUT/${e}_$q.json"
  done
  for q in 1 6 9 17 20; do
    [ -e "$OUT/${e}_q$q.json" ] || $PY bench/report/profile.py --engine $e --query $q --sf 0.1 --out "$OUT/${e}_q$q.json"
  done
done
echo done > "$OUT/../profile_all.done"
