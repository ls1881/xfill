#!/usr/bin/env bash
# Runs xfill_cli against each grid in benchmarks/grids/ and reports timing.
# TODO: flesh out once solver + CLI timing output exist.
set -euo pipefail

BIN="${1:-./build/xfill_cli}"
DICT="${2:-data/wordlist_sample.txt}"

for grid in benchmarks/grids/*.txt; do
  echo "=== $grid ==="
  time "$BIN" "$grid" "$DICT"
done
