## Appendix

### A.1 TPC-H SF1: median times (ms)

One thread (✗: the answer did not match the reference):

{{table:tpch_sf1_t1_ms}}

16 threads:

{{table:tpch_sf1_t16_ms}}

### A.2 TPC-H SF0.1: median times (ms) and ratios

One thread:

{{table:tpch_sf0.1_t1_ms}}

{{table:tpch_sf0.1_t1_ratio}}

16 threads:

{{table:tpch_sf0.1_t16_ms}}

{{table:tpch_sf0.1_t16_ratio}}

{{img:tpch_sf0.1_geomean|TPC-H SF0.1: geometric-mean time relative to DuckDB at 1 and 16 threads}}

### A.3 Operators and H2O-style workloads at 16 threads

{{table:micro_t16_ms}}

{{table:micro_t16_ratio}}

{{table:standing_micro_t16}}

{{table:h2o-g1_t16_ms}}

{{table:h2o-j1_t16_ms}}

### A.4 Engine configuration

Every engine ran with its defaults except what is listed. "Threads" is the only knob varied.

| Engine | Threads set by | Other non-default settings |
|---|---|---|
| cdb | `Database(threads)` | none (compression and SIMD on; the optimizer on) |
| DuckDB 1.5.6 | `SET threads = N` | none; in-memory database; `DECIMAL(15,2)` money columns in the "(DECIMAL)" rows only |
| DataFusion 54.1.0 | `SessionConfig.with_target_partitions(N)` and `TOKIO_WORKER_THREADS=N`; tables registered as `MemTable`s with N partitions | none |
| ClickHouse 26.9.2.1 (chDB 4.4.0) | `SET max_threads = N` | `join_use_nulls = 1`, `aggregate_functions_null_for_empty = 1` (SQL-standard results), `input_format_csv_trim_whitespaces = 0` (load); `Memory` tables, and `MergeTree ORDER BY tuple()` for the variant |
| Polars 2.0.0 | `POLARS_MAX_THREADS=N` | none; the 22 queries of `pola-rs/polars-benchmark` at commit `401908a` minus `.round(2)`, with Q11's constant fraction; SQL interface for the micro-benchmark and H2O-style workloads |
| SQLite 3.53.4 | single-threaded | `:memory:`; the TPC-H primary- and foreign-key indexes of `py_worker.py` and `ANALYZE`; queries rewritten by `make_sqlite_queries.py` |

### A.5 Software and data

Python 3.14.7; GCC 13.3 (`-O3 -DNDEBUG`, no `-march=native`) for cdb; the packages of `tools/requirements-bench.txt`
(`duckdb==1.5.6`, `datafusion==54.1.0`, `chdb==4.4.0`, `polars==2.0.0`, `pyarrow==25.0.1`, `numpy==2.5.3`, `pandas==3.0.6`,
`matplotlib==3.11.2`). TPC-H data: DuckDB's `dbgen` at scale factors 0.1 and 1 written to pipe-delimited CSV by `tools/tpch_data.py`
(the same files for every engine; `DECIMAL(15,2)` columns read as doubles). Micro-benchmark and H2O-style data:
`bench/report/workloads.py gen`, fixed seed 0.42.

### A.6 The raw results

`bench/report/results/{{n:results_date}}/`: one JSON-lines file per campaign stage (`tpch_sf1_t1.jsonl`, `micro_t16.jsonl`, ...;
one record per visit: every run time and CPU time of every query, the load times, peak memory, the control probes, the CPU the rest
of the machine used, flags and retries), the noise repeats (`noise_*.jsonl`), the verification results (`verify_*.json`,
`check_*.json`), the optimizer-quality, durability and scaling probes, the callgrind profiles (`profiles/`), the memory-bandwidth
measurement (`membw.txt`) and the optimizer ablation (`ablation_sf0.01.md`).

### A.7 The tie band of every configuration

τ is the 90th percentile, over queries and engines, of the relative interquartile range of the per-round ratio to DuckDB (§5.4):

{{table:tau}}
