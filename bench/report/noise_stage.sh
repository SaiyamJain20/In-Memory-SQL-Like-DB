#!/bin/sh
# One noise-floor measurement: the same query on the same engine, repeated, on the machine as it is
# (docs/REPORT.md, "Environment and noise"). Usage: noise_stage.sh <tag> [results dir]
TAG=${1:?tag}
OUT=${2:-bench/report/results/$(date +%F)}
PY=.venv-bench/bin/python
mkdir -p "$OUT"
F="$OUT/noise_$TAG.jsonl"
for e in cdb duckdb datafusion chdb polars; do
  $PY bench/report/driver.py noise --engine $e --workload tpch --sf 1 --query 6 --threads 1 --repeats 50 --out "$F"
done
for e in cdb duckdb; do
  $PY bench/report/driver.py noise --engine $e --workload tpch --sf 1 --query 1 --threads 1 --repeats 30 --out "$F"
  $PY bench/report/driver.py noise --engine $e --workload tpch --sf 1 --query 6 --threads 16 --repeats 50 --out "$F"
done
echo done > "$OUT/noise_$TAG.done"
