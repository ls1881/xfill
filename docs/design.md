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

2. ✅ **Benchmark harness** — `benchmarks/run_benchmarks.sh` against the
   grids in `benchmarks/grids/` (generated with
   `benchmarks/generate_grid.py`), capturing nodes/backtracks/time.
3. ✅ **Performance passes**, in the order actually applied (see
   `docs/bibliography.md` for the sources each is drawn from):
   - Trail-based incremental backtracking instead of full domain copies
     (only the domains a decision actually touches get snapshotted).
   - Queue-based AC-3 with a subset-check and an all-letters-viable fast
     path, run at every node instead of a one-time root pass -- this is
     what made real cascading propagation affordable everywhere, not
     just at the root.
   - `dom/wdeg` branching (domain size over a weighted degree that grows
     when a crossing causes a wipeout, decaying otherwise) in place of
     plain MRV.

   Net effect on `sample_11x11.txt` (min_score=50): 42ms → 19.6ms.
   `sample_15x15_interlock.txt` (72 words): 6ms, 0 backtracks.

4. **Not yet implemented** (see bibliography for why each is a
   reasonable next step, and what it would cost):
   - Cell-level branching with a SoCDP-style heuristic instead of
     slot-level MRV (Orca's headline architectural difference -- a
     bigger rewrite than the above, since it changes the search
     variable from "which word" to "which letter").
   - Adaptive branching stickiness and randomized restarts with
     geometric backtrack-limit growth (`ingrid_core`).
   - Nogood learning via constraint-graph clustering (Anbulagan & Botea's
     COMBUS) -- would help most on the genuinely hard, dense grids
     (`sample_13x13/15x15/21x21.txt`) that remain intractable even after
     the passes above; per Anbulagan & Botea's own phase-transition
     data, "hard region" instances can take >24h even for a dedicated
     solver, so this is a real ceiling, not a bug in this codebase.
   - Two-stage overestimation search / partial-state warm-starting
     (Botea & Bulitko) -- built for score-*optimization* crosswords, so
     porting it to our plain feasibility solver isn't a direct fit, but
     the "aggressive-prune a partial state, seed a second search with
     it" shape is transferable.
   - "Max shared substring" duplicate avoidance (`ingrid_core`'s
     n-gram-windowed `DupeIndex`) -- generalizes our exact-word-only
     uniqueness check to also forbid near-duplicate entries.

## Benchmarking philosophy

Every optimization in this repo should be justified by a profiled
before/after number on the grids in `benchmarks/grids/`, not by
asymptotic argument alone — some theoretically-superior techniques
(e.g. AC-4 support counters) can lose to a well-vectorized simpler
approach in practice due to cache locality and constant factors.
