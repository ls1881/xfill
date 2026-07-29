#!/usr/bin/env bash
# Runs xfill_cli against each grid in benchmarks/grids/ and reports timing.
set -euo pipefail

BIN="${1:-./build/xfill_cli}"
DICT="${2:-data/spreadthewordlist_caps.txt}"

for grid in benchmarks/grids/*.txt; do
  echo "=== $grid ==="
  time "$BIN" "$grid" "$DICT"
done
