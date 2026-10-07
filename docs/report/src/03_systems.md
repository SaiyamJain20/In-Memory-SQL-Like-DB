## 3. The systems compared

### 3.1 Why these five

The question is "how does a from-scratch columnar, vectorized engine compare with the engines it is modelled on and with the
standard alternatives", so the comparison set is chosen by *role*, not by availability alone:

| Engine (version) | Role in the comparison | Why it is here |
|---|---|---|
| **DuckDB** 1.5.6 | the reference | Same design family (embedded, columnar, vectorized, push-based, morsel-driven) and the correctness oracle of the whole project; every other number is read against it. |
| **Apache DataFusion** 54.1.0 | the closest architectural peer | Rust, Apache Arrow memory, a vectorized engine with its own optimizer, used as the SQL layer of many systems; the other widely used open-source embeddable columnar SQL engine. |
| **ClickHouse** 26.9.2.1 (embedded as chDB 4.4.0) | the industry reference for vectorized columnar analytics | The system ClickBench and most analytical-database comparisons are built around; chDB runs the unmodified ClickHouse engine in-process, so it can be driven exactly like the others. |
| **Polars** 2.0.0 | the DataFrame reference | The high-performance single-node DataFrame engine, benchmarked on TPC-H (`polars-benchmark`) and the H2O.ai benchmark; its queries are written against its lazy API rather than in SQL, which makes it a different kind of peer (§3.3). |
| **SQLite** 3.53.4 | the row-store baseline | The most deployed embedded database: B-tree row storage and a row-at-a-time bytecode interpreter. It shows what columnar + vectorized execution buys, and is the natural peer for durable commit latency. |

Not included, and why: PostgreSQL (needs a server install and root, which this machine does not give), Velox and Umbra / HyPer
(libraries or research systems that cannot be installed and driven like the others here), MonetDB (no maintained embedded
build). The TPC-H and H2O.ai benchmarks are the standard workloads for exactly this set of engines.

### 3.2 Design axes

What follows summarises each project's own documentation, not anything this report measured; the harness measured only the
thread counts actually used, resident memory and the timings (chapters 5 and 6).

| | cdb | DuckDB | DataFusion | ClickHouse (chDB) | Polars | SQLite |
|---|---|---|---|---|---|---|
| Language | C++20 | C++ | Rust | C++ | Rust | C |
| Kind | embedded library | embedded database | query-engine library | database server, embedded here | DataFrame library with a SQL front end | embedded database |
| Data layout | columnar; 122,880-row row groups of immutable, encoded segments | columnar; row groups | columnar Arrow record batches (no storage engine of its own) | columnar parts (MergeTree) or in-memory blocks (Memory) | columnar Arrow-compatible frames | B-tree pages of rows |
| Execution | push-based pipelines over 2,048-value vectors | push-based pipelines over vectors | pull-based streams of Arrow batches (8,192 rows by default) | pipeline of processors over blocks | lazy plan executed over columns | one row at a time in a bytecode VM |
| Parallelism | morsel-driven thread pool | morsel-driven thread pool | partitions (`target_partitions`) on an async runtime | `max_threads` pipeline streams | a global thread pool | one thread per query |
| Optimizer | rule-based plus cost-based join order from HyperLogLog statistics | rule-based plus cost-based, with statistics | rule-based, with statistics for join build side | rule- and heuristic-based | lazy rule-based (pushdowns, common-subplan elimination) | cost-based planner for indexes (statistics from `ANALYZE`) |
| Compression | lightweight per-segment encodings (frame of reference, RLE, dictionary, scaled doubles) | lightweight in database files | none in memory | codecs in MergeTree parts | none (Arrow layout) | none |
| Durability | write-ahead log + checkpoints, fsync per commit | write-ahead log + checkpoints | none | MergeTree parts on disk | none | journal / WAL |
| SIMD | AVX2 kernels, run-time dispatch | compiler + explicit | compiler (Arrow kernels) | explicit, run-time dispatch | explicit + compiler | none |

### 3.3 Where "the same query" is not the same

* **DuckDB and cdb** run the same SQL text (the queries exported from DuckDB's `tpch` extension) and both answer to the
  DuckDB answer files. DuckDB is also run with its native `DECIMAL(15,2)` money columns as a second row; the primary
  comparison gives every engine `DOUBLE` money columns, as cdb stores them (ADR 0003).
* **DataFusion** runs the same SQL text unchanged and passes all 22 queries.
* **ClickHouse** runs the same SQL text with three non-default settings, each required for SQL-standard results:
  `join_use_nulls = 1` (a `LEFT JOIN` pads with NULL, not with the type's default: TPC-H Q13 counts a non-existent order
  otherwise), `aggregate_functions_null_for_empty = 1` (an aggregate over no rows is NULL, not 0: Q17 at small scale
  factors) and `input_format_csv_trim_whitespaces = 0` (the loader must not trim the spaces TPC-H comments begin and end
  with). Two table engines are measured: `Memory` (data in RAM, the like-for-like row) and `MergeTree` (its normal storage;
  its page cache is warm).
* **SQLite** runs the same text rewritten mechanically for its dialect (`bench/report/make_sqlite_queries.py`: dates as ISO
  text, `strftime` and `substr` for `EXTRACT` and `SUBSTRING`, Q13's derived-table column list) and, as it is a
  row store without a statistics-driven join order, with the standard TPC-H primary- and foreign-key indexes (listed in
  `py_worker.py`) and `ANALYZE`; without indexes most queries would not finish.
* **Polars** runs the 22 DataFrame-API queries of `pola-rs/polars-benchmark` (Apache 2.0) with three mechanical changes that
  do not change the work (the tables come from memory, `.round(2)` formatting is removed so the answers compare exactly with
  the answer files, Q11's threshold fraction is the SQL text's constant). For the micro-benchmark and H2O-style workloads,
  which have no published Polars versions, Polars' own SQL interface runs the same SQL as the others. So the Polars rows
  measure Polars' hand-tuned DataFrame queries against everyone else's SQL: a favourable comparison for Polars by
  construction, and said so wherever it matters.
