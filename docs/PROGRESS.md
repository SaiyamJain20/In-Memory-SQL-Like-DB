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
