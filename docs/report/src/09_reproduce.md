## 9. Reproducing this report

Everything is in the repository; the raw per-visit results of the run reported here are in
`bench/report/results/{{n:results_date}}/` (JSON lines: every run time, CPU time, probe and verification result), so the tables
can be rebuilt without re-measuring.

```bash
# 1. the engine and the harness
cmake --preset release && cmake --build --preset release          # builds cdb_report_worker, cdb_tpch, cdb_persist
python3 -m venv .venv-bench && .venv-bench/bin/pip install -r tools/requirements-bench.txt

# 2. data (TPC-H by DuckDB's dbgen with the answer files; the micro-benchmark and H2O-style tables)
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements-dev.txt
for sf in 0.1 1; do .venv/bin/python tools/tpch_data.py --sf $sf; done
for w in micro h2o-g1 h2o-j1; do .venv-bench/bin/python bench/report/workloads.py gen $w; done

# 3. correctness first: every engine against DuckDB's answers
.venv-bench/bin/python bench/report/driver.py verify --workload tpch --sf 1 --threads 8 --engines cdb,duckdb,datafusion,chdb,polars
.venv-bench/bin/python bench/report/driver.py check  --workload micro --engines cdb,duckdb,datafusion,chdb,polars

# 4. the campaign (hours; resumable; keep the machine as you want to report it)
bench/report/campaign.sh bench/report/results/<date>

# 5. tables, charts, numbers and the report
.venv-bench/bin/python bench/report/analyze.py --results bench/report/results/<date> --out docs/report
.venv-bench/bin/python bench/report/make_report.py                      # docs/REPORT.md
```

`bench/report/driver.py run --workload tpch --sf 1 --threads 1 --rounds 5 --engines cdb,duckdb --out x.jsonl` runs one
configuration; `driver.py noise` repeats one query to measure the noise floor of the machine as it is.
