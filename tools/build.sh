#!/usr/bin/env bash
# Compile ESPEShell with the arduino-cli bundled in the Arduino IDE install.
# Usage: tools/build.sh [--clean]      (prints errors, then the size summary)
set -uo pipefail
CLI="${ARDUINO_CLI:-/c/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe}"
[ -x "$CLI" ] || { echo "arduino-cli not found at: $CLI (set ARDUINO_CLI)" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="${TMPDIR:-/tmp}/espeshell-build"
[ "${1:-}" = "--clean" ] && rm -rf "$CACHE"
mkdir -p "$CACHE"
out=$("$CLI" compile --build-path "$CACHE" "$ROOT" 2>&1)
rc=$?
echo "$out" | grep -E "error:|Error |warning:" | head -40
echo "$out" | grep -E "^Sketch uses|^Global variables"
exit $rc
