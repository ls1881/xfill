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

  The direct-lookup fast path's "which words are still in this slot's
  domain" list (`WordBitset::SetBits`) was itself a fresh
  `std::vector<size_t>` constructed and freed on every single popped
  queue slot, even though it already reserved its exact capacity up
  front. Given an `AppendSetBits` counterpart that fills a caller-supplied
  vector instead of returning a new one, this became a persistent scratch
  vector (`slot_candidates_scratch_`) cleared and refilled per slot rather
  than reallocated. Verified byte-identical node/backtrack counts on two
  100-grid real samples, with a further ~3% total-time win on top of the
  snapshot-pool change below (seed 42: 20.5s → ~20s; seed 7, heavier:
  63.2s → 61.1s).

  The min-domain queue pop itself had a real quadratic inefficiency: it
  recomputed `domains[i].Count()` (an O(chunks) popcount) for every still-
  queued slot on *every single pop*, even slots whose domain hadn't
  changed since the previous pop -- an O(Q) recomputation repeated Q times
  to drain a Q-slot queue, O(Q²) total. A queued slot's domain only ever
  changes inside this same function, and every such change is immediately
  followed by a call to enqueue() for that slot (or a contradiction
  return), so caching each slot's popcount at the moment it's (re-)queued
  keeps the cache valid for as long as it stays queued, turning the scan's
  per-slot cost into an O(1) lookup. Verified byte-identical node/backtrack
  counts on both 100-grid real samples, and this was the single biggest
  win of the session: seed 42 dropped from ~20s to a stable ~18s, and the
  heavier seed-7 sample from 61.1s to 55.5s.

  One more fusion followed the same pattern as the min-domain-pop cache
  above: narrowing a crossing neighbor's domain was `operator&=` (one
  chunk-array pass) followed by `Any()` (a second pass, usually short-
  circuiting early) and then, inside the subsequent re-queue, `Count()`
  (a third, full pass) -- three passes over data that had just been
  computed. `WordBitset::AndAssignCount` fuses the intersect and the
  resulting popcount into one pass; a domain that comes up empty
  (`new_count == 0`) is exactly the old contradiction case, and a nonzero
  count is already exactly what re-queuing needs, so there's no separate
  `Any()` or `Count()` call left to make. Verified byte-identical
  node/backtrack counts on both samples, with a further ~5% total-time
  win: seed 42 down to ~17s, seed 7 down to 52.7s.

  Building the per-crossing letter-mask union still started with a full
  `ClearAll()` (a memset over the scratch bitset) before OR-ing in the
  first included letter mask -- a redundant pass, since assigning that
  first mask directly gives the identical result without ever touching
  the old contents. (An earlier, more aggressive attempt at avoiding this
  clear -- restructuring the whole loop to be chunk-major -- was tried and
  reverted for a locality regression on `grid_053.txt`, described above;
  this is the narrower, letter-major-order-preserving version of that fix,
  which back when it was first tried showed no measurable difference. It
  does now: with most of the surrounding overhead already gone, this
  smaller win is no longer buried in noise.) Verified byte-identical node
  counts on both samples, with a further ~3% win: seed 42 down to ~16.4s,
  seed 7 down to 50.1s.

  That same fix, though, still copies the first included letter mask into
  `filter_scratch_by_length_` even when it's the *only* one -- a real
  memmove for no reason, since a single-letter `possible` needs no union
  at all. Since a crossing narrowed down to one viable letter is common
  (especially deep in the search, once domains are small), special-casing
  it -- checking `(possible & (possible - 1)) == 0`, the standard
  single-bit test -- lets that case intersect the neighbor's domain
  directly against the dictionary's own letter mask, skipping the copy
  into scratch space entirely; the multi-letter union-building path is
  unchanged. Verified byte-identical node counts on both samples, with a
  further ~3% win: seed 42 down to ~15.9s, seed 7 down to 48.7s.

  One more small one in the same neighborhood: `WordBitset::AppendSetBits`
  (the direct-lookup fast path's "which words survive" enumeration)
  scanned every chunk of the array unconditionally, even after it had
  already found every set bit there was -- for a narrow domain (a
  singleton is the extreme case) whose one surviving word happens to sit
  in a large dictionary's bitset, most of that scan is checking trailing
  all-zero chunks for no reason. Since the caller already knows the exact
  popcount (the same cached count from the min-domain-pop fix above), an
  optional `max_bits` argument stops the scan the moment that many bits
  are found. Verified byte-identical node counts on both samples, with a
  further, smaller ~1% win: seed 42 down to ~15.7s, seed 7 down to ~48.2s
  -- about 29% faster overall than where this session started (68.2s).
- **Backtracking.** Trail-based: assigning a slot snapshots only the
  domains that assignment actually touches, once per decision level.
  "Once per level" was originally enforced by scanning back through the
  trail from that level's start looking for an existing entry for the
  slot — an O(k) scan repeated for each of the (up to) k domains a single
  cascade touches. Replaced with an O(1) check: each Assign()-triggered
  cascade (and each root-propagation pass) draws a fresh, never-repeated
  epoch number, and every slot remembers the epoch it was last saved
  during, so "already saved this level" is one integer comparison. A
  naive version of this that used the trail-size mark itself as the
  epoch would be unsound, since that mark gets reused whenever Undo
  returns the trail to the same size for a sibling candidate at the same
  depth. Verified via the full unit test suite (including the
  no-solution-exists cases, which exercise Undo most) and, on two
  independent 100-grid real samples (seed 42 and seed 7), identical
  node/backtrack counts either way.

  Every domain snapshot this scheme (and Undo restoring it) touches is
  still a real heap allocation and a real free, though: `SaveDomainOnce`
  copies a slot's domain onto the trail (allocating a fresh buffer), and
  when Undo later restores that snapshot, the domain state it's
  overwriting gets freed. Since every domain of a given length is always
  the same size, that alloc/free pair is pure waste -- the freed buffer
  is exactly what the next snapshot at that length needs. Fixed with a
  per-length recycle pool (`snapshot_pool_by_length_`): Undo hands the
  about-to-be-discarded domain state to the pool instead of letting it be
  freed, and SaveDomainOnce pops a buffer from the pool and copies into
  it in place (no reallocation, since the size already matches) instead
  of allocating a fresh one, falling back to a real allocation only when
  the pool for that length is empty. Verified byte-identical
  node/backtrack counts on both 100-grid samples, and a real, clearly
  visible win on both: the seed-42 sample (486k total nodes across 78
  solved grids) dropped from a noisy 21.5-22.4s band down to a clean
  20.45s, and the heavier seed-7 sample (1.55M total nodes across 84
  solved grids) went from this session's starting point of 68.2s to
  63.2s -- about 7% faster overall, with profiling confirming
  malloc/free-related samples on a representative heavy grid dropped by
  roughly 80%.
- **Branching.** `dom/wdeg` (`rf-/ingrid_core`, crediting Balafoutis):
  masked domain size over summed crossing weight to unassigned
  neighbors, lowest first; crossing weights bump on wipeout and decay
  otherwise. Word choice within a slot is always score-ordered, never
  randomized. `SelectBranchSlot` scores each unassigned slot via
  `WordBitset::CountAndNot` (popcount of domain-minus-used-words without
  materializing the intersection) instead of copying the domain to mask
  it and count the result, avoiding a heap allocation on every candidate
  slot on every branching decision — verified to produce identical
  node/backtrack counts on the 100-grid real sample.

  `Backtrack`'s candidate loop had the same pattern: it copied the chosen
  slot's domain and masked out used words once per node, then tested
  membership against that copy while iterating every word of that length
  in score order. Replaced with testing `domains[slot]` and
  `used_by_length[length]` directly per candidate instead -- correct
  because `Assign`/`Undo` always round-trip both back to their
  per-iteration-start values before the next candidate is tested, so a
  reference behaves identically to the snapshot copy it replaces. Verified
  byte-identical node counts on both 100-grid real samples; unlike the
  other allocation removals this session, this one landed close to
  neutral (within the same noise band as before, not a clear win) -- kept
  anyway since it's strictly less work with no measured downside, not for
  a speed claim.
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

**Tried and reverted:** fusing `Propagate`'s per-crossing filter build,
subset-check, and clear into one chunk-major pass (iterate chunks
outermost, OR together whichever of the up to 26 letter masks apply for
each chunk). In isolation this removes two full passes over the scratch
bitset (the explicit clear and a separate subset-check read), and most
grids in the 100-grid real sample got dramatically faster this way — but
it also switches the memory access pattern from streaming fully through
one contiguous letter-mask array at a time (the original, letter-major
loop order) to touching up to 26 *different* masks' memory once per
chunk, and that regressed grids whose crossings commonly have wide
`possible` sets: `grid_053.txt` got ~35% slower and flipped from solved
to timeout. Reverted in favor of a smaller, safe change that keeps the
original letter-major loop order (no locality regression) while still
skipping the redundant clear by assigning the first included mask
directly instead of clearing then OR-ing it in — but that narrower fix
showed no measurable aggregate win either, so it wasn't kept.

**Also tried and reverted:** breaking `SelectBranchSlot`'s exact dom/wdeg
priority ties by degree (crossings to unassigned neighbors, preferring
the more-constraining slot) instead of the arbitrary lowest-slot-id
tie-break a plain `(priority, slot)` pair comparison gives. On the
seed-42 sample this looked like a clear win (78/100 solved either way,
but ~17% fewer total nodes) — but on the heavier, more reliable seed-7
sample it *regressed* solve count (84 → 82, two real losses and zero
gains, no grids flipped the other way). Same underlying pattern as the
backjumping/nogood-learning reverts above: any change to search order,
even a well-motivated one, perturbs which restart's random seed ends up
solving a given grid, and the effect isn't reliably positive across a
large real sample even when a smaller sample suggests it is. Also tried:
replacing `Propagate`'s O(total slots) min-domain scan with a sorted
`vector<int>` of just the currently-queued slots (kept in ascending
order specifically to preserve the original scan's tie-breaking exactly,
verified byte-identical on both samples) — but this was a wash-to-slight-
regression, not a win: the O(S) scan being replaced was mostly cheap
`vector<bool>` reads (the *real* per-slot cost, an O(chunks) popcount, was
already skipped for non-queued slots via `continue`, same as before), so
removing it saved little while the new sorted-insert/erase maintenance
cost was pure added overhead. Profile before assuming a scan is the
bottleneck, not just the largest loop bound in the code.

Profile-guided optimization (build an instrumented binary, run it over a
sample of real grids, rebuild using that profile) was also tried as a
build-level, behavior-preserving alternative to further code changes --
byte-identical by construction, and safe from the restart-interaction
risk above since it can't change *what* the solver does, only how the
compiler lays out the code. It showed no measurable difference, most
likely because moving `WordBitset`'s methods into the header (an earlier
session's fix) already captured the main cross-translation-unit inlining
win PGO would otherwise offer. Not adopted, since it would add a
real build-process dependency (an instrumented pre-build run) for no
measured benefit.

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
