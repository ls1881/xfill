# URTC 2026 reproducible testbench

This directory is the complete, reproducible benchmark backing the
Experiments section of `paper/xfill_urtc2026.tex`. It compares four
crossword-filling solvers (a fifth was tested and excluded, see below) on
a fixed grid set, dictionary, and timeout.

## What's compared

| Solver | Architecture | Source |
|---|---|---|
| **xfill** | restart-portfolio, dom/wdeg branching, concurrent shared conflict weights (this repo) | `build/xfill_cli` |
| **orca-solver** | partition-based parallel search | https://github.com/johnhawksley/orca-solver (v0.3.0 used here) |
| **ingrid_core** | single-threaded reference solver | https://github.com/szunami/ingrid |
| **crossword-composer** | static-order recursive backtracker, no restarts, no parallelism | https://github.com/paulgb/crossword-composer, commit `5655a3b` |

crossword-composer is included as an **unmodified naive baseline** — a
real, working, independently authored solver, but with no restart
strategy, no learned/adaptive heuristic, and no parallelism. It is not
expected to be competitive at 15x15 scale; that gap is itself part of the
point of the per-grid-size and per-grid figures in the paper (Section V).

### savin_crossword — tested and excluded

A fifth solver, savin_crossword (textbook AC-3 + backtracking, CS50-style,
no restarts) — https://github.com/SavinRazvan/crossword, commit
`9cd1b9f` — was also run through this harness (`run_savin()` in
`run_benchmark.py`) but is **excluded from `main()`'s default sweep and
from every figure**. It timed out on all 20 grids at both a 120s and a
300s cap, including the trivial 5x5 and 7x7 curated grids — ruling out an
under-provisioned timeout as the explanation. Its least-constraining-value
step recomputes an `O(|domain|^2)` score from scratch at every node,
which this testbench's ~184,000-word dictionary makes prohibitive
regardless of grid difficulty. `run_savin()` and `to_savin_structure()`
are kept in `run_benchmark.py` for reproducibility, just not called by
default; pass a grid through them directly if you want to re-verify this
yourself.

## Dictionary — public only, never the paid one

This testbench derives its dictionary **deterministically from
`data/spreadthewordlist_caps.txt`**, which is already committed to this
repository and freely available (spreadthewordlist.com). It filters to
`score >= 40` (`MIN_SCORE` in `run_benchmark.py`) and writes two files at
run time: `results/dict_min40.dict` (`WORD;SCORE`, for xfill/orca/ingrid)
and `results/dict_min40_plain.txt` (one word per line, for
crossword-composer and savin_crossword, which have no notion of word
scoring).

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

## Timeout and trials

`results.csv` was built in two passes: `run_benchmark.py` first swept
every grid at a 300-second cap, then every (solver, grid) pair that
timed out was re-tested at 600s via `rerun_timeouts.py` (`TIMEOUT_SECONDS`
in both scripts is now 600, matching the second pass). This two-stage
process exists specifically to check whether a timeout meant "needed more
budget" or "genuinely stuck," and the two are not the same: of 18 pairs
re-tested at double the cap, exactly one changed — ingrid_core went on to
solve `grid_479` in 325s. Every other timeout, including all 11 of
crossword-composer's and the one grid no solver resolves at either cap
(`sample_13x13`), held at 600s too. This cap is **not** the same as this
project's other, uncapped multi-hour benchmarks documented in
`docs/design.md`, which exist for a different purpose (finding the true
limits of one specific hard grid, not producing a reproducible
multi-solver comparison).

xfill and orca-solver both race independent workers internally (a
restart portfolio and a partition-based search, respectively), so wall
time on a given (solver, grid) pair genuinely varies run to run —
OS thread scheduling decides which worker gets there first, not just the
solver's own logic. Both are run `N_TRIALS_RANDOMIZED = 5` times per
grid; `results.csv` reports the median trial, and every individual trial
is logged to `results_trials.csv` for full transparency. ingrid_core and
crossword-composer are single-threaded with no restart logic — given the
same grid and dictionary they are deterministic, so each runs once.

### Re-running only what timed out

If you increase `TIMEOUT_SECONDS` and want to re-test just the
(solver, grid) pairs that previously timed out — without re-running (and
re-paying the wall-clock cost for) everything that already worked —
`rerun_timeouts.py` does exactly that: it reads the existing
`results.csv`/`results_trials.csv`, re-runs only the TIMEOUT entries at
the new cap, and merges the results back in place.

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
.venv/bin/python3 run_benchmark.py          # writes results/results.csv, results/results_trials.csv
.venv/bin/python3 run_xfill_strengths.py    # writes results/ablation.csv, results/thread_scaling.csv
.venv/bin/python3 generate_figures.py       # writes results/figures/*.{png,pdf}
```

## Three additional experiments, specifically isolating xfill's contribution

`run_benchmark.py`'s general sweep uses grids that mostly solve in well
under a second for the sophisticated solvers -- too easy for the effects
below to show up. `run_xfill_strengths.py` runs three more targeted
experiments:

1. **Shared-conflict-weight ablation, known-hard grids**
   (`results/ablation.csv`, `ablation()`): xfill run twice per grid,
   identical in every respect except the `XFILL_DISABLE_SHARED_WEIGHTS`
   environment variable (a benchmarking-only hook in `SolveParallel`, see
   `src/solver.cpp` -- unset, the default, changes nothing), on
   `HARD_GRIDS`, a fixed list reused from this project's own pre-existing
   development benchmarking (not cherry-picked for this paper). This
   isolates the paper's actual mechanism from every other difference a
   cross-solver comparison necessarily carries (different language,
   dictionary handling, everything).
2. **The same ablation, on `STANDARD_GRIDS`**
   (`results/ablation_standard.csv`, `ablation_standard_corpus()`): the
   exact same toggle, but on the same 12-grid scraped-15x15 sample every
   other scraped-grid figure in the paper uses, rather than the
   separately-curated `HARD_GRIDS` list. This closes a real gap --
   `HARD_GRIDS` was never tested against the paper's own main corpus --
   and shows the first ablation's regime-dependent finding (a real win on
   some grids, a wash or worse on others) is not an artifact of that one
   curated list: the same three patterns reappear independently on
   `grid_395`, `grid_423`, and `grid_479` (the other 9 of the 12 solve in
   well under a second regardless of the toggle and carry no signal).
   Both ablations are combined into one figure by `fig_ablation_combined`
   in `generate_figures.py`.
3. **Thread-count scaling** (`results/thread_scaling.csv`,
   `thread_scaling()`): xfill and orca-solver on `HARD_GRIDS` subset
   `SCALING_GRIDS`, across thread counts from 1 to 3x physical core
   count, showing the two architectures respond to added parallelism in
   opposite ways (Section V-C/Discussion of the paper).

## Files

- `run_benchmark.py` — the harness (self-contained, see module docstring).
- `rerun_timeouts.py` — re-runs only previously-timed-out (solver, grid)
  pairs at a new cap and merges the results back in; see "Timeout and
  trials" above.
- `generate_figures.py` — builds the figures used in the paper; each
  function's docstring states which specific claim the figure backs. An
  earlier version also produced a cumulative-solve-rate-by-time figure
  and a xfill/orca-solver overlap-count figure; both were removed once
  another figure already in the paper covered the same underlying
  numbers from a different angle -- see the `NOTE:` comments in this
  file for the specific reasoning behind each removal.
- `external_adapters/crossword_composer_cli.rs` — a headless CLI wrapper
  we wrote for crossword-composer (upstream only ships a hardcoded demo
  `main.rs` and a browser UI). Original code, MIT-compatible with the
  upstream project; not a copy of any upstream file.
- `results/results.csv` — raw output from the run backing the paper's
  numbers, committed for inspection without re-running anything.
- `results/results_trials.csv` — every individual trial for xfill and
  orca-solver (5 per grid), the raw data `results.csv`'s medians summarize.
- `results/ablation_standard.csv` — per-trial data for the second ablation
  above (`grid_395`/`grid_423`/`grid_479`, 5 trials per config).
- `results/figures/` — the generated figures, committed alongside the CSVs.
