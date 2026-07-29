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
- `Solver` — runs queue-based AC-3 propagation after each guess, branches
  using `dom/wdeg` (see step 3 below), and wraps the whole search in
  randomized restarts with a growing backtrack budget (step 4) so a
  single unlucky branch order can't stall the search forever. See the
  README's "How the algorithm works" section for the full walkthrough,
  and `include/xfill/solver.hpp`'s class comment for the most detailed,
  most up-to-date version of this description.

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

4. ✅ **Randomized restarts with geometric backtrack-limit growth**
   (`ingrid_core`'s `find_fill`/`find_fill_for_seed`, motivated by Gomes,
   Selman & Kautz's heavy-tailed-runtime-distributions result -- see
   `docs/bibliography.md`): when a search attempt racks up more dead ends
   than its budget (starting at 500, ×1.1 per retry), it aborts and
   restarts from the root with a fresh RNG seed and a bigger budget,
   keeping the dom/wdeg crossing weights learned so far. Slot choice on
   restarts is a weighted-random pick among the best few dom/wdeg-ranked
   slots (`kRandomSlotWeights = {4, 2, 1}`) instead of always the single
   best, so different attempts actually explore different branch orders.
   The first attempt stays fully deterministic (greedy dom/wdeg) --
   benchmarking showed randomizing it too regresses grids the greedy
   choice already solves well. Word choice within a slot is never
   randomized, to preserve the score-quality-first fill behavior.

   See `docs/bibliography.md` for exactly what was and wasn't ported from
   `ingrid_core` (word-choice randomization and "adaptive branching
   stickiness" were both deliberately left out) and why.

   **Tried and reverted:** Meehan & Gray's grid-seeding idea (place one
   random word in a high-degree slot before each restart, since a
   deterministic search otherwise repeats the identical fill attempt). It
   was implemented, then backed out after benchmarking exposed a real
   soundness bug: a seeded attempt that exhausts naturally only proves
   *that seed word* doesn't lead anywhere, not that the grid is
   unsatisfiable, but `Solve()`'s restart loop was treating any non-
   budget-aborted exhaustion as definitive. See `docs/bibliography.md`'s
   Meehan & Gray entry for the concrete false-negative this produced.

5. ✅ **Real-world benchmark set + profiling-driven propagation speedups.**
   `benchmarks/grids/scraped_15x15/` holds 500 real 15x15 grid layouts
   (block patterns only) scraped from crosswordgrids.com via
   `benchmarks/scrape_crosswordgrids.py`, and
   `benchmarks/bench_subset.py` runs a reproducible random sample of
   them (fixed seed) with a per-grid timeout, for measuring real
   before/after effect rather than tuning against the small hand-picked
   set in `benchmarks/grids/` alone. A 20-grid sample (seed 42) mostly
   *didn't* solve at all under `min_score=50` with a 20s cap (5/20
   solved) -- real newspaper-style 15x15s are considerably denser and
   harder than this project's synthetic grids, which is exactly why they
   were worth benchmarking against.

   `sample`-profiling one of the timeouts (`grid_013.txt`) showed ~97% of
   samples inside `Propagate`'s letter-viability check
   (`WordBitset::Intersects`, `Dictionary::LetterMask`,
   `WordBitset::operator|=`) -- i.e. the search wasn't spending time on
   backtracking bookkeeping at all, it was propagation-bound. Two fixes
   followed directly from that profile, both purely mechanical (no new
   algorithm, no behavior change -- every fix below produces byte-identical
   node/backtrack counts on `benchmarks/grids/`, just faster):
   - `WordBitset::SetBits()` rewritten to skip zero chunks and extract set
     bits via `ctz` + clear-lowest-bit, instead of testing every index
     one at a time -- it was an accidental O(size()) bit-by-bit scan
     where it should have been an O(chunks + popcount) one.
   - `Propagate`'s "which letters are viable at this crossing" check gets
     a direct-lookup fast path for domains below `kDirectLookupThreshold`
     candidates (tuned to 1000 by rerunning `bench_subset.py` at several
     values): read the actual surviving words' letters via the now-fast
     `SetBits()` instead of testing all 26 `LetterMask`s against the full
     bitset, since an `Intersects()` call's cost depends on where the
     surviving words fall in the array, not on how few of them remain.
   - The per-crossing `filter` bitset is now a reused per-length scratch
     buffer (`filter_scratch_by_length_`) instead of a fresh heap
     allocation every crossing, once `sample` showed malloc/free churn
     as a real (~15%) cost too.

   Net effect on the same 20-grid sample: every grid that solved before
   is 2.3-4.8x faster (e.g. `grid_112.txt`: 0.100s → 0.021s), one former
   20s timeout (`grid_126.txt`) now solves in ~7s, and the existing
   curated grids sped up similarly with *identical* nodes/backtracks
   (e.g. `sample_9x9.txt`: 0.22s → 0.05s, still 1449 nodes/202
   backtracks) -- confirming these are pure constant-factor wins, not
   search-order changes.

6. ✅ **Component-restricted branching** (Dechter's "non-separable
   components" -- see `docs/bibliography.md`): `Solver` computes connected
   components of the slot-crossing graph once (one BFS pass in the
   constructor), and `SelectBranchSlot` only offers candidates from the
   lowest-indexed component that still has an unassigned slot, fully
   settling one before starting the next. Free (identical node/backtrack
   counts) on every single-component grid, which is every grid in
   `benchmarks/grids/` and, it turns out, all 500 in
   `benchmarks/grids/scraped_15x15/` too -- real crosswords are built to
   avoid weak interlock, so they're essentially always non-separable. A
   constructed stress test (4 tiled copies of `sample_9x9.txt` as
   independent components) confirms the mechanism itself works: 323,978
   nodes/9.33s → 125,521 nodes/3.59s, ~2.6x fewer nodes. Kept as a real,
   sound, zero-cost safety net (e.g. for
   `benchmarks/grids/synthetic/disconnected_15x15.txt`, or any future
   grid with genuinely independent regions), not because it moves the
   needle on the currently-timing-out real grids -- see the bibliography
   entry for the full investigation, including checking all 500 real
   grids for articulation points (485/500 have none at all).

   **Tried and reverted:** graph-based backjumping (Dechter 1990). Fully
   and soundly implemented (jump to the most recent assigned crossing
   neighbor of an exhausted slot instead of always the immediate parent;
   see `docs/bibliography.md` for the dynamic-ordering soundness
   adaptation this needed). All 15 tests passed, but the 20-grid real
   sample went from 6 solved to 4 -- `grid_016.txt`/`grid_126.txt` no
   longer finish in 20s even though `sample_11x11.txt` got 4x faster.
   Reverted on the aggregate result despite the individual win. Likely
   cause: it overrides dom/wdeg's own tuned choice of what to try next,
   and duplicates what randomized restarts already handle (escaping a
   stuck attempt) via a different, conflicting mechanism.

7. **Not yet implemented** (see bibliography for why each is a
   reasonable next step, and what it would cost):
   - Cell-level branching with a SoCDP-style heuristic instead of
     slot-level MRV (Orca's headline architectural difference -- a
     bigger rewrite than the above, since it changes the search
     variable from "which word" to "which letter").
   - Nogood learning via constraint-graph clustering (Anbulagan & Botea's
     COMBUS) -- confirmed still needed: `sample_13x13.txt` and the
     original `sample_15x15.txt` (fully-open interior, not
     `sample_15x15_interlock.txt`) were re-tested with restarts in place
     and still didn't finish within a 5-minute cap at `min_score=50`.
     Randomized restarts (step 4) help with heavy-tailed *bad luck* in the
     search order, but a hard-region instance in Anbulagan & Botea's sense
     is expensive for *every* search order, which only nogood learning
     (not restarts) can address. Per their own phase-transition data,
     "hard region" instances can take >24h even for a dedicated solver, so
     some ceiling here is real, not a bug in this codebase.
     (`sample_21x21.txt` is not actually in this category -- see the
     README's "Known limits" section: it's proven unsatisfiable in
     microseconds by domain size alone, not a hard search.)
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
