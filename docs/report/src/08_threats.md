## 8. Threats to validity and limitations

**The machine was shared, not isolated.** Chapter 5 states how, and how the effect was bounded; it was not removed. The
governor was `powersave` and could not be changed, so CPU frequency moved with load and with the desktop. Ratios between
engines measured in the same round are the defensible quantity; absolute times are specific to this session. Effects
smaller than the tie band (§5.4) are not claimed. At 16 threads the benchmark competes with the desktop for the same cores,
and the noise floor is correspondingly higher; the one-thread results are the headline comparison for that reason.

**One machine, one CPU generation, one operating system.** AVX2 without AVX-512, 8 cores with SMT, 14 GB of memory. Engines
that depend on wider SIMD, on more memory bandwidth or on more cores may rank differently elsewhere. Scale factors 0.1 and 1
({{n:sf3_note}}) fit in memory but are small by the standards of the benchmark (SF100 and SF1000 are the usual
published sizes); behaviour that appears only when data does not fit in memory (spilling, buffer management) is out of scope
by construction, and cdb does not spill at all.

**TPC-H-derived, not TPC-H.** The queries are the standard 22 with the substitution parameters of DuckDB's `tpch`
extension, run on `dbgen` data. This is not a TPC-certified result, and the benchmark's own rules (refresh streams, power and
throughput tests, price/performance) are not followed. For all engines money columns are `DOUBLE` in the primary comparison;
DuckDB's exact `DECIMAL(15,2)` is reported separately (§6.9).

**Defaults, not tuning.** Every engine ran with its default configuration except the thread count and the ClickHouse settings
that SQL semantics require. A tuned configuration (different batch sizes, table engines, sort keys, indexes) could move any
engine, in particular ClickHouse, whose `MergeTree` is designed around a sort key and a primary index that these queries do
not use. SQLite ran with the standard primary- and foreign-key indexes because it cannot run the queries without them.

**Polars is measured with hand-written DataFrame queries.** They are the public, community-optimised implementations, not SQL;
for the SQL-only workloads Polars' SQL interface is used instead, which translates to the same engine but is a different
front end. The Polars column therefore answers "how fast is Polars on well-written queries", not "how fast is Polars on
the same SQL text".

**The H2O.ai-style data was re-created.** The generator of the original benchmark is an R script; the tables here follow the
published description (cardinalities, value ranges, ten and five questions) and use a fixed seed, so the results are
comparable between engines in this report but not with published H2O.ai numbers.

**Estimator quality is judged on 22 queries.** q-error statistics over 22 TPC-H queries describe TPC-H's well-behaved,
uniform, key-foreign-key data; they say little about skewed or correlated data. DuckDB's estimates are read from its JSON
profile and cdb's from `EXPLAIN ANALYZE`; scans differ in what they report (DuckDB's scan includes a pushed-down filter, cdb's
filter is a separate operator), so the per-kind q-error table keeps them apart and the plan-quality comparison uses the rows
produced by the joins (C_out), which both engines define the same way.

**The estimator comparison favours cdb.** cdb's estimator was developed by running `EXPLAIN ANALYZE` on these 22 queries
at these scale factors and fixing what it showed (§6.8); DuckDB's was not tuned to them. A fair test of estimate quality needs
queries the estimator has not seen, which this report does not have.

**Timing includes the client call.** Python engines are timed around a Python call (`fetch_arrow_table`, `collect`, ...) and cdb
around `Connection::Query` in a C++ process; the difference is microseconds, which is negligible for queries of milliseconds and not
for the sub-millisecond ones (`scan_count` at 0.5–1.4 ms on some engines).

**No hardware counters.** `perf` is not installed and cannot be installed without root, so the explanations in chapter 7
rest on controlled experiments and on callgrind (instruction counts and a cache simulation), not on measured cache or branch
miss rates.

**Engine-version dependence.** Everything here is for the pinned versions of `tools/requirements-bench.txt`; DuckDB,
DataFusion and ClickHouse improve quickly and the gaps will move.

**The author built one of the systems.** The harness, the choice of queries and the interpretation were written by the person
and assistant that wrote cdb. The mitigations are structural rather than a promise: every engine's answers are checked
against a third party's answer files (DuckDB's) before any timing counts; the harness code, queries, raw per-run results and
the one-command reproduction are in the repository; engine-specific query changes are mechanical and listed; and results in
which cdb loses are kept in the tables with the same prominence as the others.
