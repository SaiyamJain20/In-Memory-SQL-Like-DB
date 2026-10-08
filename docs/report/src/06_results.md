## 6. Results

How to read the tables. Times are medians over rounds of the median of three warm runs, in milliseconds, unless a table
says otherwise; ratios are *time of the engine ÷ time of DuckDB*, taken within each round and summarised by their median, so
**below 1 is faster than DuckDB**. "≈" marks a ratio that is not distinguishable from 1 (its 95% interval contains 1, or it
is within the tie band τ of its configuration, §5.4). ✗ marks a result that failed verification (§6.1). Intervals in
brackets are 95% bootstrap intervals over rounds. Everything in this chapter is regenerated from the raw files by
`bench/report/analyze.py`.

### 6.1 Correctness

Before any timing is reported, every engine's answer to every query was compared with the reference (DuckDB's answer files for
TPC-H; DuckDB's fresh answers on the same data for the other workloads) at every thread count that was timed.

{{table:correctness}}

Every engine returns the reference answer for every query it can express, with two exceptions that are about the engines
and not about the harness:

* **DataFusion and ClickHouse return no row for TPC-H Q15 at 8 and 16 threads** (they are correct on one thread). Q15 asks
  for the supplier whose revenue *equals* the maximum revenue, and the revenue is the same parallel floating-point sum
  computed twice; with more than one thread the two sums differ in their last bits, so the equality finds nothing. cdb had the
  same defect until Phase 8, when it made `SUM` / `AVG` of doubles order-independent (compensated accumulation, rounded once,
  at a cost of about 30% on TPC-H Q1 against the build before it, `BENCHMARKS.md`); DuckDB and Polars return the right row at every thread count. The timing of
  Q15 for the two affected engines is reported (the work done is the same) and marked ✗.
* **Polars on Q17 at SF0.01** (a dry run, not part of the timed results) returned `0` where the reference has `NULL`: a sum
  over no rows is `0` in Polars. At the timed scale factors Q17 matches.

The H2O-style groupby questions have no `ORDER BY`, so their answers are compared as sets of rows.

### 6.2 TPC-H, scale factor 1, one thread

One thread is the headline comparison: the engines do the same work on the same core, and it is the configuration least
disturbed by the desktop (§5.4). τ = {{n:tau_sf1_t1_pct}}%.

{{img:tpch_sf1_t1_ratio|TPC-H SF1, one thread: time per query relative to DuckDB (dots to the left of the line are faster than DuckDB; the grey band is the tie band)}}

{{table:tpch_sf1_t1_ratio}}

{{img:tpch_sf1_geomean|TPC-H SF1: geometric-mean time relative to DuckDB at 1 and 16 threads}}

* **cdb is {{n:sf1_t1_ratio_cdb}}× DuckDB's time** [{{n:sf1_t1_ratio_cdb_lo}}, {{n:sf1_t1_ratio_cdb_hi}}] over the 22
  queries: slower on {{n:standing_sf1_t1_duckdb_loss}}, tied on {{n:standing_sf1_t1_duckdb_tie}} and faster on
  {{n:standing_sf1_t1_duckdb_win}} (the geometric mean of the times is {{n:sf1_t1_gm_cdb}} ms against {{n:sf1_t1_gm_duckdb}} ms).
  That is the same order as the 2.31× that the simpler single-engine harness of `docs/BENCHMARKS.md` measured two days earlier.
* The other engines, against DuckDB: DataFusion {{n:sf1_t1_ratio_datafusion}}×, **Polars {{n:sf1_t1_ratio_polars}}×**
  (parity, on hand-written DataFrame queries), ClickHouse {{n:sf1_t1_ratio_chdb}}×, SQLite {{n:sf1_t1_ratio_sqlite}}× (with
  indexes). **On one thread cdb is the slowest of the five columnar and DataFrame engines**: it takes
  {{n:standing_sf1_t1_datafusion_ratio}}× DataFusion's time, {{n:standing_sf1_t1_polars_ratio}}× Polars' and
  {{n:standing_sf1_t1_chdb_ratio}}× ClickHouse's (faster than ClickHouse on {{n:standing_sf1_t1_chdb_win}} queries, tied on
  {{n:standing_sf1_t1_chdb_tie}}, slower on {{n:standing_sf1_t1_chdb_loss}}); it is ahead only of SQLite, which needs
  {{n:sf1_t1_sqlite_over_cdb}}× cdb's time even with indexes.

{{table:standing_sf1_t1}}

* **Where cdb is far from DuckDB** (ratio to DuckDB): Q17 {{n:r_tpch_sf1_t1_cdb_q17}}×, Q9 {{n:r_tpch_sf1_t1_cdb_q09}}×,
  Q20 {{n:r_tpch_sf1_t1_cdb_q20}}×, Q11 {{n:r_tpch_sf1_t1_cdb_q11}}×, Q13 {{n:r_tpch_sf1_t1_cdb_q13}}×, Q2
  {{n:r_tpch_sf1_t1_cdb_q02}}×, Q8 {{n:r_tpch_sf1_t1_cdb_q08}}×, Q5 {{n:r_tpch_sf1_t1_cdb_q05}}×. **Where it is close:** Q14
  ({{n:r_tpch_sf1_t1_cdb_q14}}) and Q19 ({{n:r_tpch_sf1_t1_cdb_q19}}) are ties, and the scan-and-aggregate queries Q6
  ({{n:r_tpch_sf1_t1_cdb_q06}}×), Q1 ({{n:r_tpch_sf1_t1_cdb_q01}}×), Q12 ({{n:r_tpch_sf1_t1_cdb_q12}}×) and Q18
  ({{n:r_tpch_sf1_t1_cdb_q18}}×) are within a third. The pattern (the scan-heavy queries are near parity, the gaps are in
  queries with correlated subqueries, many joins or big sorts) is the subject of chapter 7.
* **ClickHouse is an outlier on the multi-join queries**: Q5 is {{n:r_tpch_sf1_t1_chdb_q05}}× DuckDB's time, Q8
  {{n:r_tpch_sf1_t1_chdb_q08}}× and Q9 {{n:r_tpch_sf1_t1_chdb_q09}}×, against {{n:sf1_t1_ratio_chdb}}× overall. Its default
  configuration was used throughout (§5.3), and it most likely joins in the order the query is written; this was not
  investigated, and a hand-tuned ClickHouse would be closer.

Which engine is fastest on how many queries, one thread:

{{table:fastest_sf1_t1}}

The full per-query times are in appendix A.1. Cold runs (the first execution in a fresh process) cost little more than warm ones:

{{table:cold_warm_sf1_t1}}

### 6.3 TPC-H, scale factor 1, 16 threads, and scaling

τ = {{n:tau_sf1_t16_pct}}%. The desktop competes for these cores, so the intervals are wider (§5.1).

{{img:tpch_sf1_t16_ratio|TPC-H SF1, 16 threads: time per query relative to DuckDB}}

{{table:tpch_sf1_t16_ratio}}

* **cdb is {{n:sf1_t16_ratio_cdb}}× DuckDB's time** [{{n:sf1_t16_ratio_cdb_lo}}, {{n:sf1_t16_ratio_cdb_hi}}] at 16 threads:
  closer than on one thread because it scales better. Of the 22 queries it is tied on {{n:standing_sf1_t16_duckdb_tie}},
  slower on {{n:standing_sf1_t16_duckdb_loss}} and faster on {{n:standing_sf1_t16_duckdb_win}}. DataFusion
  {{n:sf1_t16_ratio_datafusion}}×, ClickHouse {{n:sf1_t16_ratio_chdb}}×, Polars {{n:sf1_t16_ratio_polars}}×. cdb against
  DataFusion: {{n:standing_sf1_t16_datafusion_ratio}}× (a tie overall: {{n:standing_sf1_t16_datafusion_win}} faster,
  {{n:standing_sf1_t16_datafusion_tie}} tied, {{n:standing_sf1_t16_datafusion_loss}} slower); against ClickHouse
  {{n:standing_sf1_t16_chdb_ratio}}×; against Polars {{n:standing_sf1_t16_polars_ratio}}×.
* The queries where cdb remains far behind at 16 threads are the same ones as on one thread (Q17 {{n:r_tpch_sf1_t16_cdb_q17}}×,
  Q4 {{n:r_tpch_sf1_t16_cdb_q04}}×, Q21 {{n:r_tpch_sf1_t16_cdb_q21}}×, Q9 {{n:r_tpch_sf1_t16_cdb_q09}}×, Q20
  {{n:r_tpch_sf1_t16_cdb_q20}}×, Q13 {{n:r_tpch_sf1_t16_cdb_q13}}×); Q4 and Q21 are newly slow because they do not scale
  (§7.3 shows where their CPU goes). It is faster than DuckDB on Q14 ({{n:r_tpch_sf1_t16_cdb_q14}}×).

**Scaling.** Geometric-mean time over the 22 queries, and the speedup over one thread (SF1):

{{img:scaling_sf1|TPC-H SF1: geometric-mean time and speedup over one thread for 1 to 16 threads}}

{{table:scaling_sf1}}

cdb has the best scaling of the engines in this comparison ({{n:speedup16_cdb}}× on 16 threads, of which 8 are physical
cores) against DuckDB's {{n:speedup16_duckdb}}×, Polars' {{n:speedup16_polars}}×, DataFusion's {{n:speedup16_datafusion}}× and
ClickHouse's {{n:speedup16_chdb}}×; it reaches DataFusion at 8 threads and is {{n:sf1_t8_ratio_cdb}}× DuckDB's time there. But
it does so by using more CPU: {{n:cpu_s_cdb_t16}} CPU-seconds for one pass over the 22 queries at 16 threads against DuckDB's
{{n:cpu_s_duckdb_t16}} ({{n:cpu_ratio_t16}}×; on one thread {{n:cpu_s_cdb_t1}} against {{n:cpu_s_duckdb_t1}}, {{n:cpu_ratio_t1}}×): the
inefficiencies of §7.2 and, at 16 threads, the waiting of §7.3.

{{table:efficiency_sf1}}

### 6.4 Scale factor 0.1, and how time grows with the data

{{table:standing_sf0.1_t16}}

At SF0.1 (a 100 MB dataset) with 16 threads **cdb is {{n:sf0.1_t16_ratio_cdb}}× DuckDB's time** [{{n:sf0.1_t16_ratio_cdb_lo}},
{{n:sf0.1_t16_ratio_cdb_hi}}]: faster than DuckDB on {{n:standing_sf0.1_t16_duckdb_win}} of 22 queries, and
{{n:standing_sf0.1_t16_datafusion_ratio}}× DataFusion's, {{n:standing_sf0.1_t16_chdb_ratio}}× ClickHouse's. On one thread at SF0.1
it is back to {{n:sf0.1_t1_ratio_cdb}}× DuckDB. The measured reason is parallel efficiency at small sizes: DuckDB's effective
parallelism (CPU time / wall time) at 16 threads is {{n:par_duckdb_sf0.1_t16}} at SF0.1 against {{n:par_duckdb_t16}} at SF1, and
`lineitem` at SF0.1 is only five row groups; an engine that hands out row groups as units of work cannot use more than five
threads on its biggest table, while cdb splits row groups into morsels of eight vectors. (That DuckDB's unit of work is the row
group is an inference from this pattern and its documentation, not something this harness tested.)

How a query's time grows when the data grows tenfold (geometric mean of the 22 queries; a flat 10× would be linear):

{{table:sf_scaling}}

cdb's one-thread time grows {{n:growth_cdb_t1}}× for 10× the data, more than linear, where DuckDB, DataFusion, ClickHouse and Polars
grow {{n:growth_duckdb_t1}}–{{n:growth_polars_t1}}×. Possible reasons, not separated here: at SF0.1 the data (about 100 MB, a
fraction of it touched by a query) is closer to the 16 MB last-level cache than at SF1, and cdb's hash tables and sorts are the
operations that suffer most when they stop fitting (the 1M-row-build join of §7.2 has {{n:dlmr_ratio_join_1m}}× DuckDB's
simulated last-level misses); the others' sub-linear growth also reflects fixed per-query costs that matter at small scale.
SF3 was not run: the desktop did not leave the memory it needs.

### 6.5 Resources: loading, memory and storage

{{img:load_memory_sf1|TPC-H SF1 on one thread: time to load the eight tables from CSV and peak resident memory}}

{{table:load_memory_sf1}}

* **Memory.** cdb has the smallest footprint of all engines on one thread: peak {{n:rss_cdb_t1}} MB resident against
  DuckDB's {{n:rss_duckdb_t1}}, DataFusion's {{n:rss_datafusion_t1}}, Polars' {{n:rss_polars_t1}} and ClickHouse's
  {{n:rss_chdb_t1}} MB (SQLite {{n:rss_sqlite_t1}}), because its stored data is compressed ({{n:stored_mb_sf1}} MB for a raw
  size of 1,408 MB, {{n:stored_ratio}}×) and its scans are zero-copy. At 16 threads cdb's peak is {{n:rss_cdb_t16}} MB
  (per-thread build state), still the smallest.
* **Loading.** cdb is the second slowest loader: {{n:load_cdb_t1}} s on one thread against DuckDB's {{n:load_duckdb_t1}} s,
  DataFusion's {{n:load_datafusion_t1}}, Polars' {{n:load_polars_t1}} and ClickHouse's {{n:load_chdb_t1}} s (SQLite
  {{n:load_sqlite_t1}} s, a Python loop). A `COPY` into cdb parses, encodes every segment, builds zone maps and a HyperLogLog
  sketch, none of which the in-memory formats of DataFusion, Polars or ClickHouse's `Memory` engine pay at load time; at 16
  threads cdb loads in {{n:load_cdb_t16}} s ({{n:load_ratio_cdb}}× faster).

### 6.6 Operators

The micro-benchmarks isolate one mechanism each on one 10-million-row table (§5.2). τ = {{n:tau_micro_t1_pct}}% at one thread.

{{img:micro_t1_ratio|Operator micro-benchmarks, one thread: time per query relative to DuckDB}}

{{table:micro_t1_ratio}}

{{table:standing_micro_t1}}

At one thread cdb is {{n:micro_t1_ratio_cdb}}× DuckDB's time over the {{n:micro_t1_queries}} queries (DataFusion
{{n:micro_t1_ratio_datafusion}}×, ClickHouse {{n:micro_t1_ratio_chdb}}×, Polars {{n:micro_t1_ratio_polars}}×: DuckDB is the slowest
of the four on single operators), and at 16 threads {{n:micro_t16_ratio_cdb}}× (the 16-thread table is in appendix A.3). Against
DataFusion, ClickHouse and Polars cdb is slower on {{n:standing_micro_t1_datafusion_loss}}, {{n:standing_micro_t1_chdb_loss}} and
{{n:standing_micro_t1_polars_loss}} of the {{n:micro_t1_queries}} queries. The picture by mechanism, against DuckDB:

| Mechanism | cdb against DuckDB (1 thread) | Reading |
|---|---|---|
| plain scan, sum and expression | `scan_sum` {{n:r_micro_t1_cdb_scan_sum}}×, `scan_expr` {{n:r_micro_t1_cdb_scan_expr}}× | **faster than DuckDB**: zero-copy scans of encoded columns and a vectorized sum (DataFusion, ClickHouse and Polars are faster still) |
| `LIKE`, string equality on a dictionary | `filter_like` {{n:r_micro_t1_cdb_filter_like}}×, `filter_str_eq` {{n:r_micro_t1_cdb_filter_str_eq}}× | **faster**: the predicate runs once per distinct dictionary entry |
| group by strings | `agg_str` {{n:r_micro_t1_cdb_agg_str}}× | **faster** |
| filters of rising selectivity | 1% {{n:r_micro_t1_cdb_filter_1pct}}×, 10% {{n:r_micro_t1_cdb_filter_10pct}}×, 50% {{n:r_micro_t1_cdb_filter_50pct}}×, 90% {{n:r_micro_t1_cdb_filter_90pct}}× | the more rows pass, the slower cdb is relative to DuckDB: the compensated sum over the surviving rows is {{n:top0_pct_filter_90pct}}% of the instructions at 90% selectivity (§7.2) |
| `count(*)` | `scan_count` {{n:r_micro_t1_cdb_scan_count}}× | **slower**: the profile shows cdb decoding a bit-packed column to count rows; DuckDB's {{n:ms_micro_t1_duckdb_scan_count}} ms for 10 million rows implies it does not scan at all |
| hash aggregation with many groups | `agg_1k` {{n:r_micro_t1_cdb_agg_1k}}×, `agg_100k` {{n:r_micro_t1_cdb_agg_100k}}×, `agg_1m` {{n:r_micro_t1_cdb_agg_1m}}×, `agg_2keys` {{n:r_micro_t1_cdb_agg_2keys}}× | slower, and worse as groups grow |
| `COUNT(DISTINCT)` | `distinct_100k` {{n:r_micro_t1_cdb_distinct_100k}}×, `distinct_str` {{n:r_micro_t1_cdb_distinct_str}}× | slower on integers, a tie on strings |
| hash join probe | `join_1k` {{n:r_micro_t1_cdb_join_1k}}×, `join_100k` {{n:r_micro_t1_cdb_join_100k}}×, `join_1m` {{n:r_micro_t1_cdb_join_1m}}× | **slower, 4.5× to 12×**: 10 million probe rows cost cdb {{n:ms_micro_t1_cdb_join_1k}} ms against DuckDB's {{n:ms_micro_t1_duckdb_join_1k}} ms even when the build side has 1,000 rows |
| sorting | `topn_10` {{n:r_micro_t1_cdb_topn_10}}×, `sort_5m` {{n:r_micro_t1_cdb_sort_5m}}× | **slower, and the largest single-query gap in the report**: the top-10 of 10 million rows takes {{n:ms_micro_t1_cdb_topn_10}} ms against DuckDB's {{n:ms_micro_t1_duckdb_topn_10}} ms |

**How close are the scans to the hardware?** The same machine reads memory at {{n:membw_read_1}} GB/s on one thread and
{{n:membw_read_16}} GB/s on 16 (`membw.cpp`: a 1 GiB array summed with four accumulators, best of seven). Expressed as logical
bytes (the width of the columns a query must read) per second, as a share of the one-thread (or 16-thread) read bandwidth:

{{table:bandwidth}}

A scan that reads compressed or dictionary-coded data can exceed 100% of this figure, which is the point of encoding; a
scan that does not is bounded by it. The interesting entries are where an engine is far below it.

### 6.7 H2O.ai-style groupby and join

The ten groupby and five join questions on 10 million rows (re-created data, §5.2). cdb has no `median`, `stddev`, `corr` or
window functions, so groupby questions 6, 8 and 9 are `n/a` for it and the geometric mean covers the {{n:h2o-g1_t1_queries}}
questions every engine answered.

{{table:h2o-g1_t1_ms}}

{{table:h2o-g1_t1_ratio}}

{{img:h2o-g1_t1_ratio|H2O-style groupby, one thread: time per question relative to DuckDB}}

On the groupby questions cdb is {{n:h2o-g1_t1_ratio_cdb}}× DuckDB's time at one thread and {{n:h2o-g1_t16_ratio_cdb}}× at 16 (DataFusion
{{n:h2o-g1_t1_ratio_datafusion}}×, ClickHouse {{n:h2o-g1_t1_ratio_chdb}}×, Polars {{n:h2o-g1_t1_ratio_polars}}× at one thread): the
closest suite for cdb. It is faster than DuckDB on `g1` ({{n:r_h2o-g1_t1_cdb_g1_sum_v1_by_id1}}×) and `g10`
({{n:r_h2o-g1_t1_cdb_g10_sum_v3_count_by_id1_6}}×), tied on `g2`, and slowest on `g4` ({{n:r_h2o-g1_t1_cdb_g4_mean_v1v2v3_by_id4}}×:
averages of three columns over 100 groups, where the compensated `AVG` of §7.5 dominates) and `g5`
({{n:r_h2o-g1_t1_cdb_g5_sum_v1v2v3_by_id6}}×: sums of three columns over 100,000 groups).

{{table:h2o-j1_t1_ms}}

The join questions are **cdb's weakest suite in this report: {{n:h2o-j1_t1_ratio_cdb}}× DuckDB's time at one thread and
{{n:h2o-j1_t16_ratio_cdb}}× at 16**. Joining the 10-million-row table against a 10-row table (`j1`) takes cdb {{n:ms_h2o-j1_t1_cdb_j1_small_inner}} ms and
DuckDB {{n:ms_h2o-j1_t1_duckdb_j1_small_inner}} ms; against a 10,000-row table (`j2`) {{n:ms_h2o-j1_t1_cdb_j2_medium_inner}} against
{{n:ms_h2o-j1_t1_duckdb_j2_medium_inner}} ms. Only the 10-million against 10-million join (`j5`, which is memory-bound for every
engine) comes within {{n:r_h2o-j1_t1_cdb_j5_big_inner}}×. A hash table of ten rows is resident in L1, so this gap is per-probe-row *work*, not memory
latency; the profile of the 1,000-row-build join (§7.2) shows the same shape.

### 6.8 Optimizer quality

Estimated against actual rows, per operator, for the 22 TPC-H queries at SF1 (cdb: `EXPLAIN ANALYZE`; DuckDB: its JSON
profile; q-error = max(estimate/actual, actual/estimate), 1 is exact):

{{table:qerror}}

{{img:qerror|Estimated vs actual rows per operator: median (dark) and 90th percentile (light) q-error}}

On this workload **cdb's estimates are far closer than DuckDB's** (median q-error over all operators {{n:qerr_cdb_all_median}} against
{{n:qerr_duckdb_all_median}}; for joins {{n:qerr_cdb_join_median}} against {{n:qerr_duckdb_join_median}}; the worst cdb estimate is
{{n:qerr_cdb_all_max}}× off, on Q18's `HAVING sum(l_quantity) > 300`, DuckDB's {{n:qerr_duckdb_all_max}}×). This
needs two warnings. First, **the comparison is biased towards cdb**: its estimator was developed and debugged by running
`EXPLAIN ANALYZE` on exactly these 22 queries and this data, DuckDB's was not tuned to them. Second, accurate estimates are not
the same as good plans:

{{table:cout}}

C_out is the total number of rows all joins of a plan produce (a plan-quality measure independent of operator speed). cdb's plan
produces fewer intermediate join rows than DuckDB's on {{n:cout_queries_cdb_smaller}} queries and more on
{{n:cout_queries_cdb_larger}} (the geometric mean of the ratios is {{n:cout_ratio_gm}}, dominated by a handful of extreme
values, so the counts are the better summary). Where cdb's plan is worse (chiefly Q2, Q5, Q9, Q11, Q16, Q20) the queries have subqueries or six-way joins, and most of
them are also among those in which cdb is slowest relative to DuckDB in §6.2, which chapter 7 builds on. Without the optimizer (cross products with filters on top, as the binder emits them), {{n:ablation_dnf}} of the 22
queries do not finish in 45 s even at SF0.01:

{{table:ablation}}

### 6.9 Variants: exact decimals, ClickHouse's storage engine, cold runs

{{table:variants_sf1}}

DuckDB with its native exact `DECIMAL(15,2)` columns is within {{n:variant_duckdb-decimal_t1}}× of DuckDB with `DOUBLE`
columns, so the choice of `DOUBLE` for the primary comparison (which is what cdb stores) does not distort it. ClickHouse's
`MergeTree` storage (its normal table engine; warm page cache) is {{n:variant_chdb-mergetree_t1}}× slower than its `Memory`
engine on one thread, which is why the in-memory row is the like-for-like comparison and the first one reported. Cold runs
(the first execution in a fresh process) cost {{n:coldwarm_cdb}}× the warm time for cdb, {{n:coldwarm_duckdb}}× for DuckDB,
{{n:coldwarm_chdb}}× for ClickHouse and {{n:coldwarm_polars}}× for Polars.

### 6.10 The noise floor

{{img:noise|Run-to-run noise of TPC-H Q6 at SF1 on one thread}}

{{table:noise}}

Single runs of the same query on the same engine, one thread: the p10–p90 spread is {{n:noise_spread1_min}}–{{n:noise_spread1_max}}% of
the median (the longer queries, Q1 at 240–300 ms, are at the low end; the shortest, 15–45 ms, at the high end), and no engine
shows a trend between the start, the middle and the end of the campaign. The mean CPU frequency during these runs, in the last
column, moved between {{n:noise_ghz_min}} and {{n:noise_ghz_max}} GHz. With 16 threads the p10–p90 spread of a 5–8 ms query
is {{n:noise_spread16_min}}–{{n:noise_spread16_max}}%: the reason the one-thread results are the headline.

### 6.11 Durability and ingest

{{table:durability}}

Single-row `INSERT`s in autocommit mode on the NVMe disk with each engine's durable setting (an fsync before the statement
returns), TPC-H SF1 as the bulk load. cdb commits in {{n:commit_ms_cdb}} ms ({{n:commit_per_s_cdb}} per second), SQLite in
{{n:commit_ms_sqlite}} ms ({{n:commit_per_s_sqlite}} per second), DuckDB in {{n:commit_ms_duckdb}} ms, ClickHouse in
{{n:commit_ms_chdb}} ms (every `INSERT` creates a part directory). The commit cost of cdb and SQLite is the cost of one fsync on this
disk (about half a millisecond): neither batches commits, and cdb has no group commit. A bulk load of the eight tables into a
persistent database, including the checkpoint (SQLite's WAL checkpoint, DuckDB's `CHECKPOINT`, ClickHouse's `OPTIMIZE ... FINAL`),
takes {{n:bulk_s_cdb}} s for cdb, {{n:bulk_s_duckdb}} s for DuckDB, {{n:bulk_s_chdb}} s for ClickHouse and {{n:bulk_s_sqlite}} s for
SQLite (a Python loop). cdb writes every loaded row twice, raw into its log ({{n:cdb_log_mb}} MB) and encoded into the checkpoint
(the "on disk" column); the files are smallest for DuckDB ({{n:size_mb_duckdb}} MB), which compresses harder than cdb's lightweight
encodings ({{n:size_mb_cdb}} MB; ClickHouse {{n:size_mb_chdb}}, SQLite {{n:size_mb_sqlite}} MB). **Reopening is where the designs differ most**: DuckDB answers Q6 {{n:reopen_s_duckdb}} s after
opening the file, which suggests that it reads pages on demand, while cdb has to load the whole database into memory first
({{n:reopen_s_cdb}} s) — the price of an engine that is in-memory first and scans zero-copy afterwards.
