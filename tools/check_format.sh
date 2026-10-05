#!/usr/bin/env bash
# Verifies (default) or applies (--fix) clang-format over all C++ sources.
# The clang-format version is pinned in tools/requirements-dev.txt; set CLANG_FORMAT to override
# the binary (e.g. CLANG_FORMAT=.venv/bin/clang-format).
set -euo pipefail
cd "$(dirname "$0")/.."
CF="${CLANG_FORMAT:-clang-format}"
mapfile -t files < <(find src tests bench tools -type f \( -name '*.h' -o -name '*.cpp' \) 2>/dev/null | sort)
if [[ ${#files[@]} -eq 0 ]]; then echo "no C++ files"; exit 0; fi
if [[ "${1:-}" == "--fix" ]]; then
  "$CF" -i "${files[@]}"
  echo "formatted ${#files[@]} files"
else
  "$CF" --dry-run -Werror "${files[@]}"
  echo "format OK (${#files[@]} files)"
fi
