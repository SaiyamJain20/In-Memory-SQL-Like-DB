#!/usr/bin/env bash
# The local "definition of done" gate: formatting plus the full test suite under every build
# configuration. Exits non-zero (and says which step failed) on the first problem.
#
#   tools/verify.sh            # format, debug, asan, tsan, release, clang-18
#   tools/verify.sh quick      # format + debug only
#
# Deliberately written with explicit exit-code checks rather than relying on `set -e`, which is
# suppressed when a script is invoked from inside an `&&` list or a harness wrapper.
cd "$(dirname "$0")/.." || exit 2
mode="${1:-full}"
CLANG_FORMAT="${CLANG_FORMAT:-$PWD/.venv/bin/clang-format}"
export CLANG_FORMAT

fail() { echo "VERIFY FAILED: $1"; exit 1; }

# The TPC-H differential tests need generated data; the gate must not skip them silently.
export CDB_REQUIRE_TPCH=1
if [[ ! -f data/tpch-sf0.01/manifest.json ]]; then
  echo "generating TPC-H SF0.01 data and DuckDB reference answers..."
  .venv/bin/python tools/tpch_data.py --sf 0.01 >/dev/null 2>&1 || fail "TPC-H data missing (needs .venv with duckdb: see CLAUDE.md)"
fi

tools/check_format.sh >/dev/null 2>&1 || { tools/check_format.sh 2>&1 | head -20; fail "format"; }
echo "format        OK"

run_preset() {
  local p="$1"
  cmake --preset "$p" >/dev/null 2>&1 || fail "configure $p"
  cmake --build --preset "$p" >build/verify-$p.log 2>&1 || { tail -30 "build/verify-$p.log"; fail "build $p"; }
  local out
  out=$(ctest --preset "$p" -j8 2>&1)
  if [[ $? -ne 0 ]]; then echo "$out" | grep -E "Failed|\*\*\*|Subprocess" | head -20; fail "tests under $p"; fi
  echo "$p $(echo "$out" | grep -E 'tests passed')" | sed 's/100% tests passed, 0 tests failed out of/OK:/'
}

mkdir -p build
run_preset debug
if [[ "$mode" == "quick" ]]; then echo "VERIFY (quick) PASSED"; exit 0; fi
run_preset asan
run_preset tsan
run_preset release

if [[ ! -f build/clang-debug/build.ninja ]]; then
  cmake -S . -B build/clang-debug -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 \
    -DCMAKE_CXX_FLAGS="--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13" \
    -DCMAKE_BUILD_TYPE=Debug -DCDB_WERROR=ON >/dev/null 2>&1 || fail "configure clang"
fi
cmake --build build/clang-debug >build/verify-clang.log 2>&1 || { tail -30 build/verify-clang.log; fail "build clang"; }
out=$(ctest --test-dir build/clang-debug -j8 2>&1) || { echo "$out" | grep -E "Failed|\*\*\*" | head; fail "tests under clang-18"; }
echo "clang-18 $(echo "$out" | grep -E 'tests passed')" | sed 's/100% tests passed, 0 tests failed out of/OK:/'
echo "VERIFY PASSED"
