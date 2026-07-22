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

1. **Correctness baseline** — implement `Grid::ComputeSlots` /
   `ComputeCrossings`, `Dictionary::LetterMask`, and a naive
   backtracking `Solver::Solve` (no propagation yet). Get tests passing
   end-to-end on the sample wordlist.
2. **Add AC-3 propagation** — filter crossing slot domains after every
   assignment, not just at leaf nodes.
3. **Benchmark harness** — wire up `benchmarks/run_benchmarks.sh` against
   real grid files, capture nodes/backtracks/time.
4. **Performance passes** (see benchmarking data before committing to
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
