#!/usr/bin/env bash
# Runs a libFuzzer target for N seconds against its seed corpus.
#   tools/run_fuzz.sh parser 60
# Build first:  cmake --preset fuzz && cmake --build --preset fuzz
# New interesting inputs are written to build/fuzz/corpus-<target> (not the committed seeds).
set -euo pipefail
cd "$(dirname "$0")/.."
target="${1:?usage: run_fuzz.sh <target> [seconds]}"
seconds="${2:-60}"
bin="build/fuzz/fuzz/${target}_fuzz"
[[ -x "$bin" ]] || { echo "build it first: cmake --preset fuzz && cmake --build --preset fuzz"; exit 2; }
work="build/fuzz/corpus-${target}"
mkdir -p "$work"
exec "$bin" -max_total_time="$seconds" -timeout=5 -rss_limit_mb=2048 -print_final_stats=1 \
    "$work" "fuzz/corpus/${target}"
