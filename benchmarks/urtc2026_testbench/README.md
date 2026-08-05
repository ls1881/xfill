# URTC 2026 reproducible testbench

This directory is the complete, reproducible benchmark backing the
Experiments section of `paper/xfill_urtc2026.tex`. It compares five
crossword-filling solvers on a fixed grid set, dictionary, and timeout.

## What's compared

| Solver | Architecture | Source |
|---|---|---|
| **xfill** | restart-portfolio, dom/wdeg branching, concurrent shared conflict weights (this repo) | `build/xfill_cli` |
| **orca-solver** | partition-based parallel search | https://github.com/johnhawksley/orca-solver (v0.3.0 used here) |
| **ingrid_core** | single-threaded reference solver | https://github.com/szunami/ingrid |
| **crossword-composer** | static-order recursive backtracker, no restarts, no parallelism | https://github.com/paulgb/crossword-composer, commit `5655a3b` |
| **savin_crossword** | textbook AC-3 + backtracking (CS50-style), no restarts | https://github.com/SavinRazvan/crossword, commit `9cd1b9f` |

crossword-composer and savin_crossword are included as **unmodified naive
baselines** — real, working, independently authored solvers, but with no
restart strategy, no learned/adaptive heuristic, and no parallelism. They
are not expected to be competitive at 15x15 scale; that gap is itself the
point of Figure 1 in the paper (Section V).

## Dictionary — public only, never the paid one

This testbench derives its dictionary **deterministically from
`data/spreadthewordlist_caps.txt`**, which is already committed to this
repository and freely available (spreadthewordlist.com). It filters to
`score >= 40` (`MIN_SCORE` in `run_benchmark.py`) and writes two files at
run time: `results/dict_min40.dict` (`WORD;SCORE`, for xfill/orca/ingrid)
and `results/dict_min40_plain.txt` (one word per line, for the two naive
solvers, which have no notion of word scoring).

**This project elsewhere also benchmarks against a second, paid
dictionary (XwiWordList) blended with spreadthewordlist — that dictionary
is never committed to this repository and is not used anywhere in this
testbench**, specifically so that everything here is reproducible by
anyone who clones the repo, with no paid content required.

## Grids

- `benchmarks/grids/curated/` — a size-graded set, 5x5 through 21x21,
  already committed.
- `benchmarks/grids/scraped_15x15/` — a corpus of real, previously
  published 15x15 grids, already committed. `run_benchmark.py` draws a
  fixed-seed (`RANDOM_SEED = 20260807`) random sample of 12 of these, so
  the exact sample is reproducible without re-downloading anything.

## Timeout

30 seconds per (solver, grid) pair (`TIMEOUT_SECONDS`), enforced by
killing the subprocess. This is a deliberately short, uniform cap chosen
so the full sweep finishes in well under an hour on a laptop — it is
**not** the same as this project's other, uncapped multi-hour benchmarks
documented in `docs/design.md`, which exist for a different purpose
(finding the true limits of one specific hard grid, not producing a
reproducible multi-solver comparison).

## Reproducing from scratch

```bash
# 1. Build xfill (from the repo root)
cmake -B build && cmake --build build --target xfill_cli

# 2. Populate external_solvers/ (gitignored -- not vendored into this repo)
mkdir -p benchmarks/urtc2026_testbench/external_solvers
cd benchmarks/urtc2026_testbench/external_solvers

git clone https://github.com/johnhawksley/orca-solver.git
(cd orca-solver && cargo build --release)

git clone https://github.com/szunami/ingrid.git ingrid_core
(cd ingrid_core && cargo build --release)

git clone https://github.com/paulgb/crossword-composer.git
cp ../external_adapters/crossword_composer_cli.rs crossword-composer/src/bin/cli.rs
# crossword-composer's Cargo.toml pulls in an old wasm-bindgen that no
# longer builds; drop the wasm/browser-UI half (this repo's src/bin/cli.rs
# is a from-scratch headless adapter that doesn't need it -- see comments
# at the top of that file for exactly why):
#   - delete src/lib.rs
#   - trim Cargo.toml to just [package]/[dependencies] (no wasm deps)
(cd crossword-composer && cargo build --release --bin cli)

git clone https://github.com/SavinRazvan/crossword.git savin_crossword

# 3. Run
cd ..
python3 -m venv .venv && .venv/bin/pip install matplotlib numpy pypdf
python3 run_benchmark.py            # writes results/results.csv
.venv/bin/python3 generate_figures.py   # writes results/figures/*.{png,pdf}
```

## Files

- `run_benchmark.py` — the harness (self-contained, see module docstring).
- `generate_figures.py` — builds the three figures used in the paper;
  each function's docstring states which specific claim the figure backs.
- `external_adapters/crossword_composer_cli.rs` — a headless CLI wrapper
  we wrote for crossword-composer (upstream only ships a hardcoded demo
  `main.rs` and a browser UI). Original code, MIT-compatible with the
  upstream project; not a copy of any upstream file.
- `results/results.csv` — raw output from the run backing the paper's
  numbers, committed for inspection without re-running anything.
- `results/figures/` — the generated figures, committed alongside the CSV.
