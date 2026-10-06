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
queries are 2.7-4.9x. *Hypothesis (confirmed in Phase 5, see below):* the join build side is a chained table (bucket
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


---

## Phase 5 - compression and SIMD kernels

Same machine, compiler and build as above (GCC 13.3 `-O3 -DNDEBUG`, **no `-march=native`**: every AVX2 function
carries a per-function target attribute and is only called after a runtime CPU check, with a scalar
fallback and a `CDB_NO_SIMD` switch). "Scalar" below is the same entry point with SIMD switched off.
Kernel micro-benchmarks: `build/release/bench/cdb_bench --benchmark_filter='Unpack|Offsets|Select|SumInt|SumDouble|MinMax|Hash.*Column|Scan(Date|Money|Sorted|Dictionary)' --benchmark_repetitions=3`
(medians; one 2048-row vector per iteration; "ps/row" = picoseconds per row).

### Kernels: before / after
| Kernel | Before (ps/row) | After (ps/row) | Speed-up | Notes |
|---|---:|---:|---:|---|
| bit unpack, width 3 | 836 | 203 | **4.1x** | generic loop -> width-specialised unpackers (scalar, compile-time constants) |
| bit unpack, width 8 | 808 | 187 | **4.3x** | generic loop -> width-specialised unpackers (scalar, compile-time constants) |
| bit unpack, width 12 | 835 | 255 | **3.3x** | generic loop -> width-specialised unpackers (scalar, compile-time constants) |
| bit unpack, width 17 | 871 | 300 | **2.9x** | generic loop -> width-specialised unpackers (scalar, compile-time constants) |
| bit unpack, width 24 | 852 | 323 | **2.6x** | generic loop -> width-specialised unpackers (scalar, compile-time constants) |
| bit unpack, width 33 | 877 | 606 | **1.4x** | generic loop -> width-specialised unpackers (scalar, compile-time constants) |
| select `int32 < c` (2% / 50% / 98% selectivity ~ same) | 518 | 144 | **3.6x** | AVX2 compare + permute-compaction; selectivity-independent |
| select `int64 < c` | 519 | 161 | **3.2x** |  |
| select `double < c` at 50% | 768 | 174 | **4.4x** | NaN/-0.0 order preserved with ordered/unordered predicates |
| select `double < c` at 2% | 1118 | 178 | **6.3x** | scalar version is branchy here |
| scaled-double decode (n / 10^e) | 1114 | 282 | **4.0x** | int64->double without AVX-512 via the 2^52 mantissa trick; bit-exact |
| SUM(int32) | 135 | 84 | **1.6x** |  |
| SUM(int64), overflow-checked | 259 | 137 | **1.9x** | vector overflow detection (sign trick) |
| SUM(double) | 689 | 84 | **8.2x** | scalar is a latency-bound add chain; vector re-associates (documented) |
| MIN+MAX(int32) | 236 | 44 | **5.4x** |  |
| MIN+MAX(int64) | 533 | 138 | **3.9x** | compare + blend (no 64-bit min in AVX2) |
| hash int32 column | 1062 | 471 | **2.3x** | murmur finaliser; 64-bit multiply from 3 x 32-bit multiplies |
| hash int64 column | 993 | 512 | **1.9x** |  |

**What did not help, and what was fixed.** An AVX2 version of the integer decode conversions
(`base + offset`, narrowing 64-bit lanes to 32 bits) measured *slower* than the plain loop the compiler
already vectorises (110 vs 83 ps/row), so it was deleted. The first AVX2 min/max was 3x *slower* than
scalar (664 vs 232 ps/row) because GCC kept the accumulators in stack slots (a store-to-load dependency
per iteration, caused by reducing through aliased aligned arrays); keeping them in registers and
reducing with shuffles made it 5.4x faster than scalar.

### Scanning encoded segments (per 2048-row vector)
A raw segment scans zero-copy; an encoded one decodes. Bytes/row is the segment's stored size.
| Column shape | Encoding | Stored bytes/row (raw -> encoded) | Scan raw | Scan encoded | Decode cost |
|---|---|---:|---:|---:|---:|
| dates over ~7 years | bit-packed (12 bits) | 4.0 -> 1.51 | 19 ns | 744 ns | 363 ps/row |
| prices 0.00-99999.99 (DOUBLE) | scaled double (24 bits) | 8.0 -> 3.01 | 19 ns | 1286 ns | 628 ps/row |
| ascending BIGINT key | delta bit-packed (2 bits) | 8.0 -> 0.26 | 19 ns | 949 ns | 463 ps/row |
| 7 distinct strings | dictionary (3 bits) | 16.0 -> 0.39 | 15 ns | 706 ns | 345 ps/row |

### Memory footprint on TPC-H SF1 (stored column data, `Table::MemoryUsage`)
`build/release/bench/cdb_tpch --sf 1 [--no-compression]`
| Table | Rows | Raw MB | Compressed MB | Ratio |
|---|---:|---:|---:|---:|
| lineitem | 6,001,215 | 994.7 | 337.5 | **2.9x** |
| orders | 1,500,000 | 221.1 | 108.4 | **2.0x** |
| partsupp | 800,000 | 121.8 | 111.7 | **1.1x** |
| customer | 150,000 | 32.4 | 29.1 | **1.1x** |
| part | 200,000 | 36.1 | 23.4 | **1.5x** |
| supplier | 10,000 | 1.9 | 1.9 | **1.0x** |
| **all tables** | | **1408** | **612** | **2.3x** |

Peak RSS after loading SF1: 1498 MB raw vs 720 MB compressed. The open tail of a table is never compressed (only sealed row groups are), and a column that does not compress (e.g. random comments, `*_comment` strings) stays raw.

### TPC-H SF1: Phase 4 -> Phase 5, compressed vs raw, vs DuckDB (single thread, min of 5 runs, ms)
| Query | Phase 4 (raw) | Phase 5 raw | Phase 5 **compressed** (default) | DuckDB 1-thread | compressed / DuckDB |
|---|---:|---:|---:|---:|---:|
| Q1 | 401.0 | 237.6 | **243.5** | 207.8 | 1.2x |
| Q3 | 149.8 | 101.2 | **126.9** | 51.7 | 2.5x |
| Q5 | 290.0 | 220.7 | **248.2** | 60.3 | 4.1x |
| Q6 | 43.3 | 35.6 | **44.3** | 22.9 | 1.9x |
| Q7 | 618.7 | 478.8 | **504.1** | 60.0 | 8.4x |
| Q8 | 286.6 | 244.4 | **277.5** | 35.4 | 7.8x |
| Q9 | 2488.0 | 1363.4 | **1419.9** | 226.9 | 6.3x |
| Q10 | 247.4 | 215.1 | **225.5** | 139.3 | 1.6x |
| Q12 | 117.4 | 118.5 | **130.1** | 99.3 | 1.3x |
| Q13 | 810.4 | 666.1 | **677.3** | 188.2 | 3.6x |
| Q14 | 50.7 | 38.9 | **50.1** | 36.9 | 1.4x |
| Q19 | 203.5 | 173.2 | **183.2** | 170.9 | 1.1x |
| **geometric mean vs DuckDB** | 3.1x | 2.4x | **2.6x** | | |

### TPC-H SF0.1: Phase 4 -> Phase 5, compressed vs raw, vs DuckDB (single thread, min of 5 runs, ms)
| Query | Phase 4 (raw) | Phase 5 raw | Phase 5 **compressed** (default) | DuckDB 1-thread | compressed / DuckDB |
|---|---:|---:|---:|---:|---:|
| Q1 | 41.5 | 23.9 | **24.6** | 21.7 | 1.1x |
| Q3 | 7.2 | 7.7 | **8.5** | 6.7 | 1.3x |
| Q5 | 16.6 | 17.4 | **18.8** | 6.8 | 2.8x |
| Q6 | 4.3 | 3.4 | **4.3** | 2.8 | 1.5x |
| Q7 | 43.7 | 41.2 | **42.0** | 9.0 | 4.7x |
| Q8 | 19.0 | 19.7 | **21.3** | 7.1 | 3.0x |
| Q9 | 61.5 | 56.4 | **57.8** | 19.9 | 2.9x |
| Q10 | 18.0 | 19.9 | **20.1** | 22.1 | 0.9x |
| Q12 | 11.4 | 11.7 | **12.7** | 11.9 | 1.1x |
| Q13 | 51.7 | 51.0 | **46.9** | 14.3 | 3.3x |
| Q14 | 3.4 | 2.2 | **3.6** | 4.2 | 0.9x |
| Q19 | 11.9 | 11.5 | **13.0** | 18.1 | 0.7x |
| **geometric mean vs DuckDB** | 1.7x | 1.5x | **1.7x** | | |

**Reading these numbers.** Against single-threaded DuckDB the geometric mean over the 12 queries went from
3.1x to **2.6x** at SF1 with the data 2.3x smaller (2.4x with compression off). At SF0.1 it is unchanged at 1.7x:
the working set fits in cache there, so prefetching has little to win, and decoding costs a few percent on the
short queries (individual SF0.1 queries move by +-10%, within the noise noted in Environment; Q1 gained 40%).
Most of the SF1 gain is *not* compression: it is what profiling the join-heavy queries found. Compression
costs 0-30% on scan-bound queries (decoding versus zero-copy) and is neutral on join-bound ones; the "raw"
column is the same engine without it.

What moved the numbers (measured, SF1, min ms; before -> after):
| Change | Effect |
|---|---|
| Join probe: hash and chain pointer in one entry, plus a pre-pass that looks every bucket up and prefetches candidates' entries and key cells so cache misses overlap | Q9 2329 -> 1429, Q7 639 -> 523, Q5 289 -> 245, Q3 146 -> 126 (this *confirms* the Phase 4 hypothesis that the SF1 join gap was memory latency) |
| Group-by on short string keys: hash inline strings from their 16 bytes, hash each distinct dictionary entry once, compare with `string_t::operator==` | Q1 429 -> 244 (401 in Phase 4, raw) |
| Decode and gather write into unzeroed storage, selection vectors allocated uninitialised, `KeyIndex` scratch reuse | `memset` 8-12% of Q9's instructions removed (callgrind) |
| Width-specialised unpack, AVX2 scaled-double decode, AVX2 compare-to-selection | compressed Q6 80.5 -> 44.3 (67.6 at that point with SIMD off), Q14 93 -> 50 |

Remaining gaps against DuckDB are the multi-way joins at SF1 (Q7 8.4x, Q8 7.8x, Q9 6.3x),
where row-wise build-side storage (one cache miss per payload row instead of one per payload column) and a better
join order (Q9, no `LIKE` statistics) are the next steps (Phases 6 and 8).

