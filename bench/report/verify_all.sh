#!/bin/sh
# Correctness of every engine at every configuration the campaign timed (docs/REPORT.md, 6.1):
# TPC-H against DuckDB's answer files, the other workloads against DuckDB's fresh answers.
#   bench/report/verify_all.sh <results dir>
RES=${1:?results dir}
PY=.venv-bench/bin/python
D="$PY bench/report/driver.py"
E="cdb,duckdb,datafusion,chdb,polars"
for sf in 0.1 1; do
  for t in 1 16; do
    eng="$E,duckdb-decimal,chdb-mergetree"
    [ "$t" = 1 ] && eng="$eng,sqlite"
    $D verify --workload tpch --sf $sf --threads $t --engines "$eng" --timeout 300 --out "$RES/verify_sf${sf}_t$t.json"
  done
done
for w in micro h2o-g1 h2o-j1; do
  for t in 1 16; do
    $D check --workload $w --threads $t --engines "$E" --timeout 600 --out "$RES/check_${w}_t$t.json"
  done
done
echo done > "$RES/verify_all.done"
