#!/usr/bin/env bash
# Unit-test the command-line parser (lists, pipes, tokens, expansion) natively.
#
# Same approach as test_calc.sh: the parser lives in shell.cpp, which is
# Arduino code, so slice the real functions out of it - from parseDollar() to
# the end of splitList() - and compile them against a String shim.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/espeshell-parse-test"
mkdir -p "$OUT"

CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null || {
  echo "no C++ compiler on PATH (set CXX)" >&2
  exit 2
}

awk '/^static size_t parseDollar\(/{on=1}
     on{print}
     on&&/^static void splitList\(/{last=1}
     last&&/^}/{exit}' \
    "$ROOT/shell.cpp" > "$OUT/parse_generated.inc"

lines=$(wc -l < "$OUT/parse_generated.inc")
[ "$lines" -gt 60 ] || { echo "slice failed: only $lines lines extracted" >&2; exit 2; }

"$CXX" -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter \
       -I"$OUT" -o "$OUT/test_parse" "$ROOT/tools/test_parse.cpp" || exit 1
"$OUT/test_parse"
