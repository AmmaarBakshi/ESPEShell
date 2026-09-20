#!/usr/bin/env bash
# Unit-test the bc expression parser natively.
#
# The parser lives in calc_cmds.cpp, which is Arduino code and cannot be linked
# into a host binary. Rather than keeping a second copy that would drift, slice
# the Calc struct out of the real source and compile that.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/espeshell-calc-test"
mkdir -p "$OUT"

CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null || {
  echo "no C++ compiler on PATH (set CXX)" >&2
  exit 2
}

# From "struct Calc {" to the closing "};" at column 0.
awk '/^struct Calc \{/{on=1} on{print} on&&/^\};/{exit}' \
    "$ROOT/calc_cmds.cpp" > "$OUT/calc_generated.inc"

lines=$(wc -l < "$OUT/calc_generated.inc")
[ "$lines" -gt 50 ] || { echo "slice failed: only $lines lines extracted" >&2; exit 2; }

"$CXX" -std=gnu++17 -O1 -Wall -Wextra -Wno-unused-parameter \
       -I"$OUT" -o "$OUT/test_calc" "$ROOT/tools/test_calc.cpp" || exit 1
"$OUT/test_calc"
