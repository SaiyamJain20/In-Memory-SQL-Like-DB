#!/bin/sh
# The whole measurement campaign of docs/REPORT.md, in order, resumable (a finished stage leaves
# <results>/<stage>.done and is skipped on the next start). Runs unattended; the machine stays in
# normal use (the control probes and background-CPU measurements record what it was doing).
#   bench/report/campaign.sh [results dir]
RES=${1:-bench/report/results/$(date +%F)}
PY=.venv-bench/bin/python
D="$PY bench/report/driver.py"
mkdir -p "$RES"
ENG="cdb,duckdb,datafusion,chdb,polars"
log() { echo "$(date +%T) $*"; }

stage() {  # stage <name> <command...>
  name=$1; shift
  if [ -e "$RES/$name.done" ]; then log "skip $name"; return; fi
  log "start $name"
  "$@" && touch "$RES/$name.done" || log "FAILED $name"
  log "end $name"
}

run() {  # run <stage> <workload> <sf> <threads> <rounds> <engines> [extra args]
  s=$1; w=$2; sf=$3; t=$4; r=$5; e=$6; shift 6
  stage "$s" $D run --workload "$w" --sf "$sf" --threads "$t" --rounds "$r" --engines "$e" \
        --known-dnf "$RES/known_dnf.json" --out "$RES/$s.jsonl" "$@"
}

stage noise_start sh bench/report/noise_stage.sh start "$RES"
run tpch_sf0.1_t1  tpch 0.1 1  5 "$ENG,sqlite" --engine-timeouts sqlite=120
run tpch_sf0.1_t16 tpch 0.1 16 5 "$ENG"
run tpch_sf1_t1    tpch 1   1  5 "$ENG,sqlite" --engine-timeouts sqlite=120
run tpch_sf1_t16   tpch 1   16 5 "$ENG"
stage noise_mid sh bench/report/noise_stage.sh mid "$RES"
for t in 2 4 8; do run tpch_sf1_t$t tpch 1 $t 3 "$ENG"; done
run tpch_sf1_variants_t1  tpch 1 1  3 "duckdb-decimal,chdb-mergetree"
run tpch_sf1_variants_t16 tpch 1 16 3 "duckdb-decimal,chdb-mergetree"
for w in micro h2o-g1 h2o-j1; do
  for t in 1 16; do run ${w}_t$t $w 1 $t 3 "$ENG"; done
done
stage optimizer_quality $PY bench/report/optimizer_quality.py --sf 1 --out "$RES/optimizer_quality_sf1.json"
stage ablation sh -c "$PY tools/optimizer_ablation.py --sf 0.01 --limit 45 --binary build/release/bench/cdb_tpch > $RES/ablation_sf0.01.md"
stage durability $PY bench/report/durability.py --sf 1 --out "$RES/durability.json"
if [ "$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)" -ge 9000 ]; then
  run tpch_sf3_t1  tpch 3 1  3 "$ENG"
  run tpch_sf3_t16 tpch 3 16 3 "$ENG"
else
  log "SF3 skipped: less than 9 GB available"
  echo "skipped: MemAvailable below 9000 MB at start" > "$RES/tpch_sf3.skipped"
fi
stage noise_end sh bench/report/noise_stage.sh end "$RES"
log "campaign complete"
