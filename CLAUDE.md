# CLAUDE.md

Guidance for AI coding agents (and humans) working in this repository. Read this first in every
session, then `docs/ROADMAP.md` (what to build next) and the latest entries of `docs/PROGRESS.md`
(what was last done and what is known-broken).

## What this project is
A from-scratch **columnar, vectorized SQL analytics engine** in C++20. Goal: a flagship systems
project for database-engine / infrastructure roles. Correctness is checked against DuckDB; speed is
measured on TPC-H. See `docs/ARCHITECTURE.md` for the design and `docs/adr/` for decisions.

## Commands
```bash
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan    # ASan+UBSan
cmake --preset tsan    && cmake --build --preset tsan    && ctest --preset tsan    # TSan
cmake --preset release && cmake --build --preset release && ctest --preset release # + benchmarks
tools/check_format.sh          # verify formatting;  tools/check_format.sh --fix  to apply
```
- Build dirs are `build/<preset>`. Single test: `build/debug/tests/cdb_tests --gtest_filter='Suite.Name'`.
- Python tooling (DuckDB oracle, formatter): `python3 -m venv .venv && .venv/bin/pip install -r tools/requirements-dev.txt`;
  `CLANG_FORMAT=.venv/bin/clang-format tools/check_format.sh`.
- TSan on this kernel needs ASLR off for the test process; `tests/CMakeLists.txt` wraps the test
  binary in `setarch -R` automatically for `CDB_SANITIZER=thread`.
- Compilers: GCC 13 (default) and Clang 18. **On this machine Clang needs
  `-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13`** (a GCC 14 dir without
  libstdc++ confuses it). CI passes the same flag.
- Hardware: x86-64 with **AVX2, no AVX-512**. Never use `-march=native`; hot kernels use runtime
  dispatch (`__builtin_cpu_supports`) with a scalar fallback.

## Layout
```
src/<module>/*.h|cpp   engine; include root is src/  (#include "storage/vector.h")
tests/<module>/*.cpp   GoogleTest, mirrors src/
bench/                 Google Benchmark micro-benchmarks and the TPC-H runner
tools/                 shell, scripts, requirements
docs/                  ROADMAP, ARCHITECTURE, PROGRESS (dev log), BENCHMARKS, adr/
```
Namespace `cdb`. Files `snake_case.h/.cpp`; types `PascalCase`; functions `PascalCase`; locals and
members `snake_case` (members suffixed `_`). No `using namespace` in headers, ever.

## Definition of done (every milestone)
1. Code + tests written; the **full** suite passes under `debug` *and* `asan`
   (and `tsan` once threads exist). New behaviour has new tests, including NULLs, empty input, and
   selection-vector / non-flat inputs for anything vector-shaped.
2. `tools/check_format.sh` passes. No new compiler warnings (`CDB_WERROR=ON` in presets).
3. A dated entry is appended to `docs/PROGRESS.md`: what changed, tests run + results, numbers for
   performance work (machine, compiler, command), known gaps.
4. `docs/ROADMAP.md` status and `docs/ARCHITECTURE.md` tags updated **only** for what is verified.
5. Commit (see Git), push the branch, check CI with `gh run list --branch <branch>`; fix red CI
   before moving on.

## Git workflow
- One branch per phase: `phase-N-short-name`, cut from `main`. Never commit directly to `main`.
- Small, focused commits; message = imperative summary line, then body explaining *why* and how it
  was verified. End with the trailer required by the harness (Co-Authored-By).
- Push the branch at every milestone (feature complete + tests green + PROGRESS entry). Merge to
  `main` (`--no-ff`) when the phase's exit criteria in `docs/ROADMAP.md` are verified, then push
  `main`.
- Never force-push, rewrite published history, or delete remote branches without being asked.

## Hard rules
- **Do not stop to ask for review or approval between steps or phases.** When a phase's exit
  criteria are verified, merge it, log it, and start the next phase. Stop only for a genuine
  external blocker (e.g. missing credentials), and say so.
- **Tests are in-depth, not token.** Cover the happy path, edge cases (empty, single row, full
  vector, boundaries), NULLs, error paths, and randomized/property tests against a simple
  reference implementation. A test that cannot fail is not a test.
- **Never weaken, skip, or delete a test or CI gate to get green.** If a test is wrong, fix it in a
  separate commit and say why. If something cannot be fixed now, record it in PROGRESS "Known gaps".
- **No unmeasured performance claims.** Anything in README/docs must trace to `docs/BENCHMARKS.md`.
- **No fake data or invented numbers** in docs. Mark unverified things as targets/planned.
- Kernels must be correct for: NULLs, selection vectors, constant/dictionary vectors, empty and
  full (2048) vectors, and 1-row tails. Hot loops are `noexcept` and never throw per row
  (ADR 0002).
- Check undefined behaviour by running ASan+UBSan, not by reasoning.
- Keep `docs/ARCHITECTURE.md` honest: tag sections `[implemented]` / `[planned: Phase N]`.

## Reference oracle
DuckDB (pinned in `tools/requirements-dev.txt`) is the oracle for SQL semantics and the TPC-H data
generator (`INSTALL tpch; LOAD tpch; CALL dbgen(sf=...)`). When behaviour is ambiguous, match
DuckDB (which follows Postgres) unless an ADR says otherwise.
