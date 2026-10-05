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
