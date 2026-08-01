#!/usr/bin/env bash
# Runs xfill_cli against the small curated/edge-case grids and reports
# timing. For the 500-grid real-world sample, use bench_subset.py instead --
# this script is for a quick sanity check, not a rigorous comparison.
set -euo pipefail

BIN="${1:-./build/xfill_cli}"
DICT="${2:-data/spreadthewordlist_caps.txt}"

for grid in benchmarks/grids/curated/*.txt benchmarks/grids/synthetic/*.txt; do
  echo "=== $grid ==="
  time "$BIN" "$grid" "$DICT"
done
