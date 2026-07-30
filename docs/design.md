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
- `Dictionary` — loads a wordlist, groups words by length, and precomputes
  letter-position bitmasks for O(1) domain filtering.
- `Solver` — runs queue-based AC-3 propagation after each guess, branches
  using `dom/wdeg`, and wraps the whole search in randomized restarts
  with a growing backtrack budget so a single unlucky branch order can't
  stall the search forever. See the README's "How the algorithm works"
  section for the full walkthrough, and
  `include/xfill/solver.hpp`'s class comment for the most detailed
  version of this description, and `docs/bibliography.md` for where each
  piece is drawn from.

## Implementation summary

- **Correctness baseline.** `Grid::ComputeSlots`/`ComputeCrossings`,
  `Dictionary::LetterMask`, and full AC-3-style propagation to a
  fixpoint after every guess.
- **Propagation.** Queue-based AC-3 (`rainjacket/orca-solver`): only
  slots whose domain actually shrank get re-examined, with a
  direct-lookup fast path for narrow domains
  (`kDirectLookupThreshold = 1000`) and a reused per-length scratch
  bitset instead of a fresh allocation per crossing. All per-length state
  on this path (`Dictionary::LetterMask`, the scratch bitset above) is
  indexed directly by length rather than through a hash map, and the
  propagation queue's membership buffer is a persistent scratch vector
  reused across calls, not reallocated per node — profiling a real
  timing-out grid showed both a hash lookup and a `vector<bool>`
  allocation on every single node were measurable costs in this hot path.
  Net effect on the 100-grid real sample (seed 42, `min_score=40`, 20s
  cap): identical node/backtrack counts, 78/100 solved either way, but
  ~40% less wall-clock time on the solved subset (35.9s → 21.6s total),
  with some individual grids over 5x faster.
- **Backtracking.** Trail-based: assigning a slot snapshots only the
  domains that assignment actually touches, once per decision level.
- **Branching.** `dom/wdeg` (`rf-/ingrid_core`, crediting Balafoutis):
  masked domain size over summed crossing weight to unassigned
  neighbors, lowest first; crossing weights bump on wipeout and decay
  otherwise. Word choice within a slot is always score-ordered, never
  randomized.
- **Restarts.** Geometric backtrack-budget growth
  (`kInitialBacktrackLimit = 500`, `kRetryGrowthFactor = 1.1`), motivated
  by Gomes, Selman & Kautz's heavy-tailed-runtime-distribution result.
  The first attempt is fully deterministic; restarts pick their branch
  slot via weighted-random choice among the best few dom/wdeg-ranked
  slots (`kRandomSlotWeights = {4, 2, 1}`). Crossing weights carry over
  across restarts; the search tree does not. Because the budget only
  grows, the search stays complete.
- **Component-restricted branching** (Dechter's non-separable
  components): `Solver` computes connected components of the
  slot-crossing graph once at construction, and branching only considers
  the lowest-indexed component with an unassigned slot. Free on any
  single-component grid — which is every curated grid and all 500 real
  scraped grids, since well-built crosswords are essentially always
  fully interlocked — and a real win (~2.6x fewer nodes on a
  constructed stress test) on a grid with genuinely independent regions.
- **Duplicate words.** A slot's effective domain is masked against a
  global "words of this length already used elsewhere" bitset at read
  time, rather than writing exclusions into every sibling domain on each
  assignment.

See `docs/bibliography.md`'s "Consulted for context, not adopted"
section for techniques that were implemented, benchmarked, and reverted
(graph-based backjumping, plain nogood learning, a project-original
dom/wdeg weight-seeding scheme) — none of them are part of the current
solver.

## Dictionary tuning

`min_score` (the solver's third CLI argument) controls how much of the
dictionary is available: entries below it are dropped at load time, and
a richer per-slot word supply loosens the coupling between crossing
slots' domains, which is exactly what search cost is most sensitive to.
For `data/spreadthewordlist_caps.txt`, `min_score=40` is the recommended
default (also `benchmarks/bench_subset.py`'s default) — it keeps the
solvability benefit of a broad dictionary while stopping short of score
tiers that contain visible data-corruption garbage (e.g. `NDRR`,
`PARISITES`, `ESKIMOK`) rather than just obscure-but-valid words.

## Known hard cases

A small number of real, densely-interlocked grids (e.g.
`benchmarks/grids/sample_13x13.txt`) remain unsolved even after 15+
minutes at `min_score=40`. This is an expected result, not a bug: per
Anbulagan & Botea's phase-transition study of crossword CSPs, some "hard
region" instances stay expensive under *any* search order, because the
underlying instance is hard, not because of a fixable search choice.
Restarts fix a search that got unlucky; they can't turn a genuinely hard
instance easy. Most 20-second-cap timeouts on the real benchmark set are
not in this category — they solve within a couple of minutes given a
longer budget — see the README's "Known limits" section for the
distinction on specific grids.

## Future work

Reasonable next steps, roughly in order of expected payoff for their
implementation cost:

- **Nogood learning via constraint-graph clustering** (Anbulagan &
  Botea's COMBUS). The plain (non-clustered) version of nogood learning
  was tried and reverted (see bibliography) because it changed
  restart dynamics for the worse; COMBUS's scoping of nogoods to
  independent constraint-graph regions might avoid that interaction, but
  that's untested.
- **Cell-level branching** with a SoCDP-style heuristic instead of
  slot-level MRV (Orca's headline architectural difference) — a bigger
  rewrite than anything above, since it changes the search variable from
  "which word" to "which letter".
- **Two-stage warm-starting** (Botea & Bulitko): aggressively prune a
  partial state, then seed a full search from it. Built for
  score-optimization crosswords, so porting to plain feasibility isn't a
  direct fit, but the general shape is transferable.
- **"Max shared substring" duplicate avoidance** (`ingrid_core`'s
  n-gram-windowed `DupeIndex`) — generalizes the current exact-word-only
  uniqueness check to also forbid near-duplicate entries, a real-world
  puzzle-quality constraint this project doesn't currently address.

## Benchmarking philosophy

Every optimization in this repo should be justified by a profiled
before/after number on the grids in `benchmarks/grids/` (and, for
search-affecting changes, the real sample in
`benchmarks/grids/scraped_15x15/` via `benchmarks/bench_subset.py`), not
by asymptotic argument alone — some theoretically-superior techniques
can lose to a simpler approach in practice due to cache locality and
constant factors, and some changes that prune real work still land net
negative once restart dynamics are accounted for. A negative result gets
written up with the same honesty as a positive one.
