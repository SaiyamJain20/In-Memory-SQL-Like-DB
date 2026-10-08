# bench/report: the cross-engine comparison harness

Produces `docs/REPORT.md` (cdb against DuckDB, DataFusion, ClickHouse via chDB, Polars and SQLite). Read the
report's chapters 4 and 5 first: they explain the design (one worker process per engine behind one line protocol,
rounds that rotate the engines, control probes, a measured noise floor). This file is the map of the code.

| File | Role |
|---|---|
| `cdb_worker.cpp` | the cdb worker (`THREADS`, `EXEC`, `RUN`, `DUMP`, `STORED`, `STAT`), built as `cdb_report_worker` |
| `py_worker.py` | the same protocol for DuckDB (also with `DECIMAL`), DataFusion, chDB (`Memory` and `MergeTree`), Polars and SQLite |
| `driver.py` | `verify` / `check` (answers against DuckDB's), `run` (visits and rounds), `noise` (the noise floor) |
| `workloads.py` | TPC-H, the micro-benchmarks, the H2O.ai-style groupby and join; `gen` writes the data |
| `common.py` | table definitions, answer comparison, control probes, statistics |
| `queries/polars/` | the 22 Polars queries (adapted by `make_polars_queries.py` from `pola-rs/polars-benchmark`, Apache 2.0) |
| `queries/sqlite/` | the TPC-H queries rewritten for SQLite (`make_sqlite_queries.py`) |
| `campaign.sh`, `noise_stage.sh` | the whole campaign, resumable |
| `optimizer_quality.py`, `durability.py`, `profile.py`, `membw.cpp`, `explain_examples.py` | estimates vs actuals, durability, callgrind profiles, memory bandwidth, the worked example |
| `analysis_lib.py`, `analyze.py`, `make_report.py` | raw JSON lines to tables / charts / numbers, and the report |
| `results/<date>/` | the raw results the report was built from |

```bash
python3 -m venv .venv-bench && .venv-bench/bin/pip install -r tools/requirements-bench.txt
cmake --preset release && cmake --build --preset release          # cdb_report_worker
.venv-bench/bin/python bench/report/driver.py verify --workload tpch --sf 0.01 --threads 4
bench/report/campaign.sh bench/report/results/$(date +%F)
.venv-bench/bin/python bench/report/analyze.py --results bench/report/results/<date> --out docs/report
.venv-bench/bin/python bench/report/make_report.py
```

Adding an engine means one class in `py_worker.py` (`open`, `load`, `exec_script`, `run`, `dump`) and, if its SQL differs, a
directory of rewritten queries under `queries/<engine>/`.
