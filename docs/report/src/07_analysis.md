## 7. Analysis: where cdb wins, where it loses, and why

Chapter 6 says *what* the numbers are. This chapter says why, using the strongest evidence available on this machine:
instruction-level profiles (callgrind with a simulated cache: exact counts for one warm execution of one query, on 1,000,000-row
micro-benchmark tables and TPC-H SF0.1, one thread), per-operator CPU time from `EXPLAIN ANALYZE`, and C_out. Hardware counters
are not available (§5.5), so a profile explains *instructions* and *simulated* last-level misses, not cycles: where the two do not
add up to the measured ratio, that is said. Everything marked "likely" is an inference that was not tested by changing the engine.

{{table:profile}}

{{table:profile_top}}

### 7.1 Where cdb is competitive, and why

* **Scan-bound work on encoded data.** `scan_sum` is {{n:r_micro_t1_cdb_scan_sum}}× and `scan_expr` {{n:r_micro_t1_cdb_scan_expr}}×
  DuckDB's time; TPC-H Q6 and Q1, which are scan + filter + aggregate, are {{n:r_tpch_sf1_t1_cdb_q06}}× and
  {{n:r_tpch_sf1_t1_cdb_q01}}×, within 30% of it. A segment is read without copying, the compare-with-a-constant kernel is AVX2, and
  the bit-packed doubles decode in vectors; the profile shows the effect of the encoding directly: on `scan_sum` cdb executes
  {{n:ir_ratio_scan_sum}}× DuckDB's *instructions* but only {{n:dlmr_ratio_scan_sum}}× its simulated last-level *misses*,
  because it reads less data, and it is the faster of the two.
* **Strings.** A dictionary segment evaluates `=` and `LIKE` once per distinct entry and hands the scan a dictionary vector:
  `filter_like` {{n:r_micro_t1_cdb_filter_like}}×, `filter_str_eq` {{n:r_micro_t1_cdb_filter_str_eq}}×, `agg_str`
  {{n:r_micro_t1_cdb_agg_str}}× DuckDB's time (on `filter_str_eq` it is level with ClickHouse and ahead of the others).
* **Memory.** The smallest resident footprint of any engine measured ({{n:rss_cdb_t1}} MB at SF1 on one thread), from encoded
  segments that scans read in place.
* **Parallel scaling and small data.** The best speedup at 16 threads ({{n:speedup16_cdb}}×) and, at SF0.1, the fastest
  engine on {{n:fastest_sf0.1_t16_cdb}} of the 22 queries at 16 threads (§6.4), from morsels of eight vectors and per-thread state merged by partition.
* **Estimates.** On the 22 TPC-H queries, the most accurate per-operator row estimates (§6.8), with the bias described there.

### 7.2 Why joins, aggregates and sorts are slow

The profile shows three mechanisms, each visible as one function.

**1. A generic per-row key comparison in every hash table.** `KeyComparator::StoredEqualsInput` compares a probe or group key with the
key stored in the hash table one row at a time, for each key column: a validity check, a dispatch on the physical type, a
selection-vector indirection, then the compare. It is {{n:top1_pct_join_1k}}% of the instructions of the 1,000-row-build join, {{n:top0_pct_agg_100k}}% of the
100,000-group aggregate, {{n:top1_pct_agg_1m}}% of the 1M-group aggregate, and the first or second function of Q1
({{n:top0_pct_q1}}%), Q9 ({{n:top1_pct_q9}}%) and Q17 ({{n:top0_pct_q17}}%). By DuckDB's design (its source and documentation,
which this harness did not examine) its hash tables compare through type-specialised code over whole vectors, keep a salt in each
entry that rejects most non-matches without reading the key, and, for a build side of dense integer keys, replace the hash
table by a direct-addressed array: its profile does show a `PerfectHashJoinExecutor` doing the micro-benchmark joins, whose keys
are dense integers (the H2O-style join keys are dense too, but those queries were not profiled). The consequence for the join micro-benchmarks: for the 1,000-row build cdb executes {{n:ir_ratio_join_1k}}× DuckDB's
instructions, and its time ratio is {{n:r_micro_t1_cdb_join_1k}}×; for the 1M-row build {{n:ir_ratio_join_1m}}× the instructions but
{{n:r_micro_t1_cdb_join_1m}}× the time, with {{n:dlmr_ratio_join_1m}}× the simulated last-level misses — most likely the probe walks a bigger
structure through more dependent loads. The H2O-style joins have the same shape (10 million probe rows, dense integer keys)
and show the same gap (§6.7).

**2. A comparison-based sort for top-N.** 77–81% of the instructions of `topn_10` and `sort_5m` are in one function, the generic
row comparator `SortBuffer::CompareRows` (plus the merge sort and `LogicalType::physical()` it calls per comparison). The
top-N operator prunes by sorting the buffer whenever it has grown to max(2·N, 8,192) rows, so *every* input row takes part in
a stable merge sort of an 8,192-row batch (log₂ 8192 = 13 comparisons per row). The other engines' top-N operators evidently do
less per row: DuckDB's profile is a vectorised comparison of each row against the current boundary (`DistinctGreaterThan`,
`GreaterThan`), and its 5,000,000-row sort executes a fifth of cdb's instructions. The top-10 of 1,000,000 rows executes
{{n:ir_ratio_topn_10}}× DuckDB's instructions ({{n:irm_cdb_topn_10}} M against {{n:irm_duckdb_topn_10}} M); the measured time ratio at 10,000,000 rows is
{{n:r_micro_t1_cdb_topn_10}}×, so the instruction count explains a part of it and the rest is not resolved by this profile.
The full sort is {{n:ir_ratio_sort_5m}}× the instructions and {{n:dlmr_ratio_sort_5m}}× the simulated misses (the comparator
dereferences rows by index), against a measured {{n:r_micro_t1_cdb_sort_5m}}×.

**3. Work that should not be done at all.** `count(*)` executes {{n:ir_ratio_scan_count}}× DuckDB's instructions (time ratio
{{n:r_micro_t1_cdb_scan_count}}×): the profile shows it decoding bit-packed data (`BitpackedInts::Decode`, `UnpackWidth`) of a
column to produce row counts that the segment already knows. And `SumDoubleState::UpdateUngrouped` is {{n:top0_pct_filter_90pct}}% of
the instructions of the 90%-selectivity filter-and-sum, and the compensated `SumDoubleState::Update` and `AvgState` are
{{n:top1_pct_q1}}% and {{n:top2_pct_q1}}% of Q1's: this is the price of the deterministic floating-point sum (§7.5), paid per row.

For the 1M-group aggregation cdb executes about as many instructions as DuckDB ({{n:ir_ratio_agg_1m}}×) and is
{{n:r_micro_t1_cdb_agg_1m}}× slower: memory behaviour (a hash table far larger than the caches, with a stored-key compare after
every probe) that a count of simulated last-level misses ({{n:dlmr_ratio_agg_1m}}×) does not capture.

### 7.3 Why Q4 and Q21 do not scale

Both queries are slow on 16 threads relative to DuckDB (§6.3) and gain little from 1 to 16 threads
(Q4 {{n:scal_q4_speedup}}×, Q21 {{n:scal_q21_speedup}}×). `EXPLAIN ANALYZE` on one and on 16 threads, with the CPU the
operators account for set against the CPU the process used:

{{table:scaling_probe}}

On one thread the operators account for 94–95% of the process's CPU; on 16 threads **{{n:scal_q4_t16_outside}}% (Q4) and
{{n:scal_q21_t16_outside}}% (Q21) of the CPU is spent outside any operator**: the process burns about three times the CPU for a
{{n:scal_q4_speedup}}× speedup. Q4's `EXPLAIN ANALYZE` also shows where the work is: a semi join whose build side is the
{{n:scal_q4_build_rows}} qualifying `lineitem` rows. The CPU time in the scan of `lineitem` is {{n:scal_q4_scan_cpu_ratio}}× larger on
16 threads than on one (memory bandwidth peaks at 4 threads on this machine, {{n:membw_read_4}} GB/s against {{n:membw_read_16}} at 16, §6.6, and the
threads contend), and the rest is most likely threads
waiting at the serial parts of a pipeline (the join's finalize between build and probe) rather than working — which is also
why cdb's CPU-seconds per pass at 16 threads are {{n:cpu_s_cdb_t16}} against DuckDB's {{n:cpu_s_duckdb_t16}} (§6.3).

### 7.4 Plans: where the optimizer's accurate estimates do not produce the best plan

The estimator is accurate on this workload (§6.8), but the plan space is what limits it:

* **Decorrelated aggregates run over the whole inner table.** A correlated scalar aggregate becomes a join with the aggregate
  *grouped by the correlation key* and computed over all of the inner table before the join with the few outer rows that
  survive. DuckDB's plan for Q17 (read with `EXPLAIN`) joins `lineitem` with the distinct keys of the qualifying parts first and
  aggregates only those rows. This is the structure behind Q17 ({{n:r_tpch_sf1_t1_cdb_q17}}× at one thread, the largest gap
  in the table), Q20 ({{n:r_tpch_sf1_t1_cdb_q20}}×) and Q2 ({{n:r_tpch_sf1_t1_cdb_q02}}×), and why the rows their joins produce are {{n:r_cout_q20}}× (Q20: {{n:cout_q20_cdb}} against {{n:cout_q20_duckdb}}) and
  {{n:r_cout_q02}}× (Q2) DuckDB's: the join of the full-size grouped aggregate with the outer table.
* **Join orders for Q5 and Q9** produce {{n:r_cout_q05}}× and {{n:r_cout_q09}}× DuckDB's intermediate rows, and cdb is
  {{n:r_tpch_sf1_t1_cdb_q05}}× and {{n:r_tpch_sf1_t1_cdb_q09}}× slower on them. (Not every gap is a plan gap: Q8, an eight-way join
  whose joins produce the same number of rows in both engines, is {{n:r_tpch_sf1_t1_cdb_q08}}× slower too, which is the operator
  speed of §7.2.) The dynamic-programming search is exhaustive up to 12 relations; its cost function (rows out + 2 × rows built +
  rows probed) does not distinguish a probe into a table that fits the cache from one that does not, so it may prefer a plan that
  is cheaper in rows and dearer in time: a possible reason, not tested.
* **Semi and anti joins always build the subquery side** (Q4, Q21, Q22): the small outer relation could be built and the big
  side probed. Not done.
* **Where the plan is better than DuckDB's:** fewer intermediate join rows on {{n:cout_queries_cdb_smaller}} queries (Q4, Q7, Q17, Q19, Q21,
  Q22); on Q19, whose predicate is a disjunction, cdb's join produces {{n:cout_q19_cdb}} rows and DuckDB's {{n:cout_q19_duckdb}}
  (cdb's optimizer factors the conjuncts common to the branches out of the disjunction and applies them before the join).

### 7.5 The price and the benefit of deterministic floating-point sums

cdb accumulates `SUM` and `AVG` of doubles in double-double form and rounds once, so a sum is bit-identical on any number of
threads and with or without SIMD. §6.1 shows what that buys: DataFusion and ClickHouse return no row for Q15 on 8 and 16 threads.
The profile shows what it costs: {{n:top0_pct_filter_90pct}}% of the instructions of the filtered sum are the compensated update, and the
grouped sum and average are the second and third functions of Q1 ({{n:top1_pct_q1}}% and {{n:top2_pct_q1}}%). A cheaper design exists in
principle for columns stored as scaled doubles (`n / 10^e`): an integer accumulation is exact and order-independent, and costs
no more than a plain add. cdb does not do it; this is a suggestion, not a measurement.

### 7.6 What would close the gaps, in the order the evidence ranks them

| Rank | Change | Evidence | Reaches |
|---|---|---|---|
| 1 | specialised, vectorised key comparison and a salted hash table for fixed-width keys | `StoredEqualsInput` is 13–40% of instructions in every join and aggregate profiled | Q1, Q9, Q17, Q20, every join and group-by micro-benchmark |
| 2 | a direct-addressed (perfect-hash) join for dense integer build keys | on the 1,000-row-build join cdb executes {{n:ir_ratio_join_1k}}× DuckDB's instructions; H2O joins j1–j4 are the same shape | joins with a small or dense build side |
| 3 | top-N with a boundary value; a specialised comparator for sort | 77–81% of instructions in one comparator | `topn_10`, `sort_5m`, the `ORDER BY ... LIMIT` queries |
| 4 | semi-join reduction of decorrelated aggregates; building the small side of a semi / anti join | Q17 / Q20 / Q2 C_out, DuckDB's plans | the three largest TPC-H gaps; Q4, Q21, Q22 |
| 5 | take `count(*)` from segment counts; cheaper deterministic sums | `scan_count` profile; Q1 and filtered-sum profiles | `count(*)`; Q1 and every double aggregate |
| 6 | less waiting at pipeline boundaries | 48–56% of CPU outside operators at 16 threads | scaling of Q4, Q21; CPU efficiency |

None of these was attempted for this report; they are what the measurements point at, and what the next phase of the project
would be measured against.
