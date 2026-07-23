# Design Notes

## Problem

Crossword grid filling is a constraint satisfaction problem (CSP):

- **Variables** — across and down slots.
- **Domains** — dictionary words of the matching length.
- **Constraints** — letters at crossing cells must agree.

The solver aims to either find a valid fill or prove none exists
(exhaustive search), not just find *a* fill quickly.

## Architecture

- `Grid` — parses a grid spec into slots and their crossing relationships.
- `Dictionary` — loads a wordlist, groups words by length, and (once
  implemented) precomputes letter-position bitmasks for O(1) domain
  filtering.
- `Solver` — runs constraint propagation (AC-3 style) after each guess,
  and branches on the most-constrained cell when propagation stalls.
- `BranchingHeuristic` — pluggable cell-selection strategy. Starting with
  MRV (minimum remaining values) as a correctness baseline before adding
  more sophisticated heuristics.

## Roadmap

1. ✅ **Correctness baseline** — `Grid::ComputeSlots` / `ComputeCrossings`,
   `Dictionary::LetterMask`, and a `Solver::Solve` that runs full AC-3
   style propagation to a fixpoint after every guess, with MRV branching
   and full-copy backtracking. No performance optimizations yet by
   design — this is the number every later change gets benchmarked
   against.

   Verified against a real ~280k-entry `WORD;SCORE` dictionary:
   - Small/medium grids (5x5 open, 7x7 with a typical block pattern):
     solves in well under a second.
   - A block-patterned 15x15 did **not** finish within 120s. This
     matches expectations, not a bug — naive backtracking with only
     MRV and no conflict-directed backjumping, cell-level branching,
     or delta-undo is exactly the baseline that specialized solvers
     like Orca are 5-100x faster than (see the "How Orca Works"
     writeup this project's roadmap is informed by). Getting a full
     15x15 solving quickly is the point of steps 2-4 below.

2. **Benchmark harness** — wire up `benchmarks/run_benchmarks.sh` against
   the grids in `benchmarks/grids/` (generated with
   `benchmarks/generate_grid.py`), capturing nodes/backtracks/time so
   every later change has a before/after number.
3. **Performance passes** (see benchmarking data before committing to
   any of these):
   - Delta-undo / trail-based backtracking instead of full domain copies.
   - Cell-level branching with a SoCDP-style heuristic instead of
     slot-level MRV.
   - Profile backtrack distance to evaluate whether conflict-directed
     backjumping is worth the added complexity.
   - Evaluate tree decomposition for grids with separable regions
     (e.g. independent corners).

## Benchmarking philosophy

Every optimization in this repo should be justified by a profiled
before/after number on the grids in `benchmarks/grids/`, not by
asymptotic argument alone — some theoretically-superior techniques
(e.g. AC-4 support counters) can lose to a well-vectorized simpler
approach in practice due to cache locality and constant factors.
