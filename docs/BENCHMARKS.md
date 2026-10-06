# Benchmarks

Every performance number in this repository comes from here, with the machine, build and command
that produced it. Numbers that are not in this file are not claims.

## Environment (all results below)
| | |
|---|---|
| CPU | AMD Ryzen 7 6800H (8C/16T, AVX2, no AVX-512), L1d 32 KiB, L2 512 KiB/core, L3 16 MiB |
| OS / kernel | Ubuntu 24.04, Linux 7.0 |
| Compiler | GCC 13.3, `-O3 -DNDEBUG` (`release` preset, no `-march=native`) |
| Caveats | CPU frequency scaling **enabled** (Google Benchmark warns about this); desktop with a browser running, load average 2–3 during the "after" runs and 6–9 during the "before" run. Treat differences under ~15% as noise; large ratios are real. |

Reproduce: `cmake --preset release && cmake --build --preset release &&
build/release/bench/cdb_bench --benchmark_min_time=0.3s --benchmark_repetitions=5
--benchmark_report_aggregates_only=true` (medians reported).

---

## Phase 1 — core data model micro-benchmarks
Unit = one benchmark iteration over 2048 rows (one standard vector) unless noted; "ns/row" is
time divided by rows processed.

| Benchmark | Time | Throughput | Notes |
|---|---:|---:|---|
| `ValidityMask::CountValid` (2048 bits, 10% NULL) | 101 ns | 20.3 G rows/s | word-wise popcount |
| `ValidityMask::IsValid` scalar scan (2048 rows) | 1141 ns | 1.80 G rows/s | 0.56 ns/row |
| `string_t::Compare`, inlined, differ in prefix | 805 ns / 1024 | 1.27 G cmp/s | 0.79 ns/cmp |
| `string_t::Compare`, out-of-line, 100-byte equal prefix | 5807 ns / 1024 | 176 M cmp/s | 5.7 ns/cmp (memcmp path) |
| `string_t ==`, inlined | 1454 ns / 1024 | 704 M/s | |
| `string_t ==`, out-of-line (40+ bytes) | 3921 ns / 1024 | 261 M/s | |
| `StringHeap::Add` 40-byte string | 6787 ns / 1000 | 147 M/s | 6.8 ns each (arena bump + memcpy) |
| `Arena::Allocate(24, 8)` | 2846 ns / 1000 | 351 M/s | 2.8 ns each |
| `Date::FromString("YYYY-MM-DD")` | 12.4 µs / 1024 | 82 M/s | 12 ns each |
| `Date::ToYMD` | 8.5 µs / 1024 | 121 M/s | 8.3 ns each |
| `Vector::Slice` (flat → dictionary, 1024 sel) | 519 ns | 1.97 G rows/s | O(1) data; allocates the selection |
| `Slice` + `Flatten` BIGINT (1024 rows) | 1345 ns | 761 M rows/s | gather |
| `VectorOps::Copy` flat INTEGER, 2048 rows | **86.9 ns** | 23.6 G rows/s | `memcpy` path |
| `VectorOps::Copy` flat BIGINT, 2048 rows | **172 ns** | 11.9 G rows/s | `memcpy` path |
| `VectorOps::Copy` BIGINT through 1024-entry selection | **541 ns** | 1.89 G rows/s | branch-free gather |
| `VectorOps::Copy` VARCHAR, all inlined, 2048 rows | **2406 ns** | 851 M rows/s | |
| `VectorOps::Copy` VARCHAR, 40-byte strings, 2048 rows | **20677 ns** | 99 M rows/s | dominated by heap copy |

### Optimization log: `VectorOps::Copy`
The first implementation tested validity per row inside the value-copy loop. The initial
benchmark exposed that a plain copy of 2048 INTEGERs cost 2.4 ns/row — about 50× off `memcpy`
speed. Fix: copy values without consulting validity (what a NULL slot holds is irrelevant for
fixed-width types), use `memcpy` for flat-source/no-selection, and mark validity with a
word-wise `SetRangeValid` when the source has no NULLs.

| Benchmark | Before | After | Speed-up |
|---|---:|---:|---:|
| Copy flat INTEGER ×2048 | 4824 ns | 86.9 ns | 55× |
| Copy flat BIGINT ×2048 | 4211 ns | 172 ns | 24× |
| Copy BIGINT via selection ×1024 | 2743 ns | 541 ns | 5.1× |
| Copy VARCHAR (inlined) ×2048 | 5661 ns | 2406 ns | 2.4× |
| Copy VARCHAR (40 B) ×2048 | 24264 ns | 20677 ns | 1.2× (heap-copy bound) |

Caveat: the "before" column was measured at higher system load (6–9 vs 2–3) with 3 instead of 5
repetitions, so the *exact* ratios carry noise; the order-of-magnitude gains on the `memcpy`
paths are far outside it. Correctness of the optimized path is covered by targeted tests plus the
model-based property test (copies into pre-populated destinations at random offsets) and
mutation checks (`tools/mutation_smoke.py`).

---

## Phase 2 — storage micro-benchmarks
Dataset: 2,000,000 rows × 6 columns (BIGINT id, INTEGER qty, DOUBLE price, DATE shipdate
ascending, VARCHAR flag (inlined), VARCHAR comment (~35 bytes, out-of-line)), default 122,880-row
row groups (17 groups). Same machine/build as above; system load average ≈ 5 during this run
(desktop session), CPU scaling enabled: treat sub-15% differences as noise.
Command: `build/release/bench/cdb_bench --benchmark_filter=BM_Table --benchmark_min_time=0.5s
--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` (medians).

| Benchmark | Time | Throughput | What it measures |
|---|---:|---:|---|
| `Table::Append`, 409,600 rows (200 chunks) incl. data generation | 47.3 ms | 8.7 M rows/s | builder append (growth + copy + seal); dominated by building the test strings |
| Scan 1 column (BIGINT), sum every value | 0.85 ms | 2.36 G rows/s | zero-copy scan + one pass over 16 MB |
| Scan 2 / 4 projected columns, sum column 0 | 0.91 / 0.92 ms | 2.2 G rows/s | extra projected columns cost ~nothing unless read |
| Scan 4 columns, **touching no values** | 69 µs | 28.8 G rows/s | pure scan machinery: ≈ 70 ns per 2048-row chunk (≈ 17 ns per column-vector view hand-out) |
| `shipdate >= X` pruning 0 % of groups (2 columns) | 35.0 µs | — | 17 of 17 groups scanned |
| … pruning 50 % | 17.5 µs | — | 9 of 17 scanned |
| … pruning 90 % | 5.3 µs | — | 3 of 17 scanned |
| … pruning 99 % | 1.35 µs | — | 1 of 17 scanned |
| `Table::Snapshot()` (17 groups) | 134 ns | — | copy of the group pointer list + schema |

Reading these honestly: a scan of uncompressed in-memory data is bound by memory bandwidth once a
consumer reads the values (2.4 G rows/s × 8 B ≈ 19 GB/s here), and costs almost nothing when it
does not (the views are precomputed). The numbers that matter for later phases will be those of
the operators *on top of* the scan (Phase 4) and of compressed scans (Phase 5); this table is the
baseline they will be judged against. Zone-map pruning scales as expected with the fraction of
groups skipped, with a ~1 µs floor from the scan setup.


---

## Phase 4 — TPC-H, single thread, vs DuckDB

The first end-to-end numbers: the 12 TPC-H queries this engine can run (the other 10 need subqueries,
Phase 8), **all verified to return DuckDB's answers** at SF0.01, SF0.1 and SF1
(`CDB_TPCH_SF=1 CDB_REQUIRE_TPCH=1 build/release/tests/cdb_tests --gtest_filter='*TpchDifferential*'`).
Both engines run **one thread** (this engine is single-threaded until Phase 6; DuckDB is pinned with
`PRAGMA threads=1`), on the same machine, one after the other.

| | |
|---|---|
| This engine | commit of the Phase 4 branch, GCC 13.3 `-O3 -DNDEBUG`, `build/release/bench/cdb_tpch --sf N --runs 5`; loaded from DuckDB-generated CSV (`tools/tpch_data.py`); DECIMAL columns are DOUBLE (ADR 0003) |
| DuckDB | 1.5.6 (pinned in `tools/requirements-dev.txt`), `tools/tpch_duckdb_time.py --sf N --threads 1 --runs 5`; data from its own `dbgen`, in its own columnar storage |
| What is timed | `Connection::Query` end to end (parse, bind, optimize, plan, execute, materialise the result) vs `con.execute(sql).fetchall()`. "min" and "median" are over runs 2-5; the first (cold) run is excluded. Load time is not included. |
| Memory | SF1 peak RSS after load 1.47 GB, after all queries 1.69 GB (DuckDB's footprint was not measured) |

### SF0.1 (600,572 lineitem rows)
| Query | rows | cdb min ms | cdb median ms | DuckDB 1-thread min ms | cdb / DuckDB |
|---|---:|---:|---:|---:|---:|
| Q1 | 4 | 41.5 | 41.7 | 21.7 | 1.9x |
| Q3 | 10 | 7.2 | 7.5 | 6.7 | 1.1x |
| Q5 | 5 | 16.6 | 17.4 | 6.8 | 2.4x |
| Q6 | 1 | 4.3 | 4.5 | 2.8 | 1.5x |
| Q7 | 4 | 43.7 | 44.5 | 9.0 | 4.9x |
| Q8 | 2 | 19.0 | 19.2 | 7.1 | 2.7x |
| Q9 | 175 | 61.5 | 65.3 | 19.9 | 3.1x |
| Q10 | 20 | 18.0 | 20.8 | 22.1 | 0.8x |
| Q12 | 2 | 11.4 | 11.4 | 11.9 | 1.0x |
| Q13 | 37 | 51.7 | 56.3 | 14.3 | 3.6x |
| Q14 | 1 | 3.4 | 3.5 | 4.2 | 0.8x |
| Q19 | 1 | 11.9 | 12.1 | 18.1 | 0.7x |
| **geometric mean** | | | | | **1.7x** |

### SF1 (6,001,215 lineitem rows)
| Query | rows | cdb min ms | cdb median ms | DuckDB 1-thread min ms | cdb / DuckDB |
|---|---:|---:|---:|---:|---:|
| Q1 | 4 | 401.0 | 410.5 | 207.8 | 1.9x |
| Q3 | 10 | 149.8 | 158.0 | 51.7 | 2.9x |
| Q5 | 5 | 290.0 | 301.9 | 60.3 | 4.8x |
| Q6 | 1 | 43.3 | 43.9 | 22.9 | 1.9x |
| Q7 | 4 | 618.7 | 623.5 | 60.0 | 10.3x |
| Q8 | 2 | 286.6 | 289.0 | 35.4 | 8.1x |
| Q9 | 175 | 2488.0 | 2580.9 | 226.9 | 11.0x |
| Q10 | 20 | 247.4 | 249.4 | 139.3 | 1.8x |
| Q12 | 2 | 117.4 | 119.3 | 99.3 | 1.2x |
| Q13 | 42 | 810.4 | 824.8 | 188.2 | 4.3x |
| Q14 | 1 | 50.7 | 55.3 | 36.9 | 1.4x |
| Q19 | 1 | 203.5 | 207.0 | 170.9 | 1.2x |
| **geometric mean** | | | | | **3.1x** |

SF1 load: lineitem 5.1 s, orders 1.0 s, partsupp 0.5 s (CSV parse, single-threaded).

**Reading these numbers honestly.** The geometric mean over the 12 queries is 1.7x at SF0.1 and
3.1x at SF1 - DuckDB is faster, as it should be at this stage. Scan/aggregate queries (Q1, Q6, Q12,
Q14, Q19) are within 1.2-1.9x. The gap opens with the multi-way joins at SF1 (Q7 10x, Q8 8x, Q9 11x), while at SF0.1 the same
queries are 2.7-4.9x. *Hypothesis, not yet measured:* the join build side is a chained table (bucket
heads, a `next` array, a `hashes` array and a separate key store), so a probe touches several cache lines
at random, which costs little while the tables fit in cache (SF0.1) and a lot when they do not (SF1);
a layout that keeps hash and key together in the bucket should help. Q9 is also hurt by a crude row estimate for `LIKE '%green%'` (fixed
selectivity 0.2; measured: 10,664 of 200,000 parts, 5.3%), which puts `part` too late in the join order. Both are targets for
Phase 5 (join hash table layout, hashing) and Phase 8 (statistics). The target for the project is
single-thread TPC-H within ~3x of DuckDB; the SF1 geometric mean is at that line, but individual
queries are not. These numbers are one run on a desktop with frequency scaling and other load (see
Environment); differences under ~15% are noise.

### Optimizations found by profiling (callgrind, Q9 at SF0.1)
| Change | Effect |
|---|---|
| `Vector::Reset()` no longer allocates (and zero-fills) a new buffer when it detaches from a shared one; storage is allocated on first access | `memset` fell from 35% to 12% of all instructions (8.9 G -> 2.2 G); SF0.1 min ms: Q3 9.4 -> 7.4, Q5 21 -> 17, Q8 27.8 -> 19, Q10 25.7 -> 19.6, Q14 4.4 -> 3.2 |
| Join ordering sized by distinct-value estimates (zone-map ranges) instead of relation size alone | Q5 at SF0.1: 2693 ms -> 21 ms (a 72 M-row intermediate result avoided) |
| Factoring conjuncts common to all branches of an `OR` (Q19's `p_partkey = l_partkey`) | Q19 at SF0.01: 46 s (nested loop over a cross product) -> 9 ms |
| *Tried, not kept:* leaving `SelectionVector` storage uninitialised | no measurable change, so the zero-initialised contract stays |
