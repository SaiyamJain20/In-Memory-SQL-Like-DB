# Progress log

Append-only, newest entry last. One entry per milestone: what changed, what was run and the
result, numbers where relevant, and what is still broken or missing. Entries record what was
*observed*, not what was intended.

Reference machine for all numbers in this repo unless stated otherwise: AMD Ryzen 7 6800H
(8C/16T, AVX2, no AVX-512), 14 GB RAM, Ubuntu 24.04, GCC 13.3, Clang 18.1.

---

## 2026-10-06 — Phase 0: Foundation
**Branch:** `phase-0-foundation`

**Changed**
- Retired the legacy row-store prototype (history: `a254830`); rationale in ADR 0001.
- CMake build (C++20, `cdb` library + `cdb_tests`), presets `debug` / `release` / `asan` / `tsan`,
  `CDB_WERROR`, `CDB_SANITIZER`, runtime-assert switch (`CDB_ENABLE_ASSERTS` in Debug + sanitizers).
- GoogleTest 1.15.2 via FetchContent.
- `common/`: `Error` + `ErrorCode` (ADR 0002), `CDB_CHECK` / `CDB_ASSERT` / `CDB_UNREACHABLE`,
  version constant.
- Toolchain smoke tests (concepts, `std::span`, `std::format`, `<bit>`, `std::jthread`, atomic
  wait/notify) so GCC/Clang divergence surfaces as a test failure.
- `.clang-format`, `tools/check_format.sh` (pinned clang-format 23.1.2).
- GitHub Actions: format check; gcc-13 + clang-18 × debug + release; asan and tsan.
- Docs: ROADMAP, ARCHITECTURE, ADR 0001/0002, CLAUDE.md, README.

**Verified (local)**
| Check | Result |
|---|---|
| `debug` preset (gcc 13.3), 13 tests | 13/13 pass |
| `asan` preset (ASan+UBSan, gcc 13.3), 13 tests | 13/13 pass |
| `tsan` preset (ThreadSanitizer, gcc 13.3), 13 tests | 13/13 pass |
| `release` preset (gcc 13.3, `-O3`), 12 tests (assert death test compiled out) | 12/12 pass |
| clang 18.1 Debug, `-Werror`, 13 tests | 13/13 pass |
| `tools/check_format.sh` | OK (7 files) |

**Notes / known gaps**
- Clang on this machine needs `--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13` (a GCC 14 dir
  without libstdc++ is picked otherwise). Documented in CLAUDE.md; applied in CI.
- TSan aborts on this kernel (high `vm.mmap_rnd_bits`) unless ASLR is disabled; the test binary
  is wrapped in `setarch -R` for TSan builds (see `tests/CMakeLists.txt`).
- No engine code yet; `release` + benchmarks are wired in Phase 1 when there is something to measure.

**CI (GitHub Actions, run 37373633236)** — all 7 jobs green: format, gcc-13 and clang-18 × debug
and release, asan (ASan+UBSan), tsan. The first attempt left six jobs unscheduled ("job was not
acquired by Runner of type hosted"): a GitHub runner-capacity failure, not a test failure; the
one job that did get a runner passed, and `gh run rerun --failed` then passed the other six.

**Phase 0 exit criteria met** → merged to `main`.

---

## 2026-10-06 — Phase 1: Core data model
**Branch:** `phase-1-core-data-model`

**Changed**
- `memory/`: `Buffer` (64-byte aligned, zeroed, 64 bytes of SIMD slack, ref-counted), `Arena`
  (bump allocator, stable pointers, dedicated blocks for large allocations, `Reset`).
- `types/`: `LogicalType`/`PhysicalType` (BOOLEAN, INTEGER, BIGINT, DOUBLE, DATE, VARCHAR; DATE
  shares INTEGER's physical type), 16-byte `string_t` (12-byte inline, 4-byte prefix, unsigned
  bytewise ordering), `date_t` + `Date` (proleptic Gregorian, years 1–9999, parse/format),
  `Value` (scalar with a documented total order: NaN last and equal to itself, -0.0 == +0.0).
- `vector/`: `ValidityMask` (bitmask, no storage when all valid), `SelectionVector`
  (+ shared read-only Identity/Zeros), `StringHeap`, `Vector` with FLAT / CONSTANT / DICTIONARY
  formats (`Slice`, `Flatten`, `Reference`, `Reset`, `SetConstant`, `ToUnified`, `Verify`),
  `VectorOps::Copy`, `DataChunk` (`Append`, `Slice`, `Flatten`, `Reset`).
- Tests infrastructure: shared random generators with adversarial strings/doubles; custom gtest
  `main` that makes the process non-dumpable (death tests went from ~24 s to ~1 s on this
  machine, where `core_pattern` pipes to apport and `RLIMIT_CORE=0` is ignored).
- `bench/cdb_bench` (Google Benchmark) and `tools/mutation_smoke.py`.

**Verified**
| Check | Result |
|---|---|
| `debug` (gcc 13.3, `-Werror`) | 151/151 |
| `asan` (ASan + UBSan, `halt_on_error`) | 151/151 |
| `tsan` | 151/151 |
| `release` (`-O3`; assert death tests compiled out) | 150/150 |
| clang 18.1 Debug, `-Werror` | 151/151 |
| `tools/check_format.sh` | OK (43 files) |
| `tools/mutation_smoke.py` | **15/15 mutations killed** (14 under `debug`, 1 under `asan`) |

Test design highlights: exhaustive `Date` round-trip for every day of years 1–9999 against
`std::chrono` (3.65 M days) and every `(start,count)` pair for `SetRangeValid`; `string_t`
comparison/equality against `std::string_view` over 160k random pairs engineered to collide on
prefixes and straddle the inline boundary (embedded NULs, bytes ≥ 0x80); a model-based
property test that applies 12 random structural operations (slice, flatten, reference, copy into
pre-populated destinations, reset-and-refill, set-constant) to 360 random vectors and compares
every row to a `std::vector<Value>` model after every step — including that vectors
`Reference()`d before a `Reset()` keep their old contents; death tests for every contract check.

**Found by the process (not by inspection)**
1. First benchmark showed `VectorOps::Copy` at 2.4 ns/row for plain INTEGER copies (~50× off
   `memcpy`); reworked into value-copy + validity-copy phases → 55× faster (numbers and caveats
   in `docs/BENCHMARKS.md`).
2. Mutation testing found a real test gap: nothing copied a NULL-free source over a destination
   that already contained NULLs. Added a targeted test and made the property test copy into
   pre-populated destinations at random offsets.

**Known gaps / deliberate limits**
- `Vector` capacity is capped at `kVectorSize` (2048); `Value` has no casts yet (Phase 3/4).
- No hashing of vectors yet (needed for aggregation/joins in Phase 4).
- `string_t::view()` on an inlined string points into the `string_t` object itself (documented in
  the header); Phase 4 kernels must take views from the stored element, not from a copy.

**Phase 1 exit criteria met** (tests for every format × type × null combination incl.
slice-of-slice; sanitizer clean; micro-benchmarks recorded).
