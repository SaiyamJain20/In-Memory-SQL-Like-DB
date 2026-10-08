## 1. Summary

**What was done.** cdb, a columnar, vectorized SQL analytics engine written from scratch in C++20 ({{n:loc_src}} lines of engine,
{{n:loc_tests}} lines of tests, {{n:mutants}} injected-bug mutants all caught by the tests), was compared with DuckDB (the
reference and the project's correctness oracle), Apache DataFusion, ClickHouse (embedded as chDB), Polars and SQLite on
TPC-H (all 22 queries, scale factors 0.1 and 1, 1 to 16 threads), 24 single-operator micro-benchmarks, the H2O.ai groupby and join
questions, the optimizer's row estimates against DuckDB's, and the cost of durability. Every engine ran in its own process
behind one measurement harness; every engine's answers were checked against DuckDB's before its timings counted.

**The result in one table** (time relative to DuckDB, geometric mean over the queries of the workload; below 1 is faster;
memory is the peak resident set on TPC-H SF1 at one thread):

{{table:headline}}

**Correctness.** cdb returns the reference answer to every query of every workload at every thread count tested. DataFusion and
ClickHouse do not for TPC-H Q15 at 8 and 16 threads (a parallel floating-point sum compared with itself); cdb had the same bug
until it made its sums order-independent (§6.1).

**Speed on TPC-H SF1.** On one thread cdb takes **{{n:sf1_t1_ratio_cdb}}×** DuckDB's time [{{n:sf1_t1_ratio_cdb_lo}},
{{n:sf1_t1_ratio_cdb_hi}}]: it is the slowest of the five columnar and DataFrame engines ({{n:standing_sf1_t1_datafusion_ratio}}×
DataFusion's time, {{n:standing_sf1_t1_polars_ratio}}× Polars', {{n:standing_sf1_t1_chdb_ratio}}× ClickHouse's) and ahead of
SQLite only, which with indexes takes {{n:sf1_t1_sqlite_over_cdb}}× as long. On 16 threads cdb is **{{n:sf1_t16_ratio_cdb}}×**
DuckDB's time, level with DataFusion ({{n:standing_sf1_t16_datafusion_ratio}}×), ahead of ClickHouse
({{n:standing_sf1_t16_chdb_ratio}}×) and behind Polars ({{n:standing_sf1_t16_polars_ratio}}×), because it scales better than
any of the others ({{n:speedup16_cdb}}× from 1 to 16 threads, against DuckDB's {{n:speedup16_duckdb}}×). On the smaller SF0.1
with 16 threads it is the fastest engine ({{n:sf0.1_t16_ratio_cdb}}× DuckDB's time).

**Where cdb is good.** The smallest memory footprint of every engine; scans of encoded data and string predicates on dictionary
columns (faster than DuckDB on `LIKE`, string equality and string group-by); the best parallel scaling; the most accurate
row estimates of the optimizers compared (with the bias that its estimator was tuned on these 22 queries, §6.8); and a durable
commit within {{n:commit_vs_sqlite_pct}}% of SQLite's time.

**Where it is not, and why** (§7, from instruction-level profiles, not from guesses):

* **Hash joins and hash aggregation** carry a generic per-row key comparison that is 13–40% of the instructions of every join and
  aggregate profiled, and there is no direct-addressed join for dense integer keys: the H2O-style joins are
  **{{n:h2o-j1_t1_ratio_cdb}}× DuckDB's time**, the worst geometric mean of the three suites.
* **Sorting and top-N** compare through a generic row comparator that is 77–81% of their instructions, and the top-N sorts
  8,192-row batches instead of keeping a boundary: the top-10 of 10 million rows is {{n:r_micro_t1_cdb_topn_10}}× DuckDB's time.
* **Correlated subqueries** (TPC-H Q17, Q20, Q2) are decorrelated into an aggregate over the whole inner table; Q17 is
  {{n:r_tpch_sf1_t1_cdb_q17}}× DuckDB's time.
* **Loading and reopening**: a `COPY` is the second slowest of the engines ({{n:load_cdb_t1}} s for SF1 on one thread), and a
  persistent database must be loaded whole into memory before the first query ({{n:reopen_s_cdb}} s, against DuckDB's {{n:reopen_s_duckdb}} s).
* **Scaling costs CPU**: at 16 threads about half the CPU time of Q4 and Q21 is spent outside any operator.

**What this says about the project.** A from-scratch engine, built and verified under a strict regime (differential testing
against an oracle, sanitizers, fuzzing, mutation testing, crash injection), runs the whole analytical benchmark correctly and lands
in the same class as DataFusion on TPC-H at 16 threads, within a small factor of DuckDB and Polars, with identifiable and
localised inefficiencies, each of which the profiles attribute to a specific function, not to the architecture: the
architecture's measurable strengths are the memory footprint, the parallel scaling and the compressed-string handling.

**What this report cannot say.** The machine was a desktop in normal use, so absolute times are noisy and pessimistic (§5.1), the
claims are ratios with intervals and a measured tie band (§5.4), and effects smaller than the band are not claimed. It is one machine,
two scale factors, default configurations, and TPC-H-*derived* queries. The author built one of the systems; §8 lists what was done
about that.
