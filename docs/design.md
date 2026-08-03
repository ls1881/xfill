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

  **Unsorted active-queue list for the min-domain pop.** The pop itself
  still scanned `in_queue` (a `vector<bool>` sized to the *whole grid's*
  slot count) on every single pop, to find the queued slot with the
  smallest domain -- cheap per slot, but paid unconditionally for every
  slot in the grid regardless of how many were actually queued. A
  *sorted* `vector<int>` of just the queued slots was tried for this
  before and reverted as a wash (see below) -- its sorted-insert
  maintenance cost ate the savings. This is the same underlying idea
  (scan only what's queued) with a different data structure: an
  *unsorted* flat list, since the min-scan already has to visit every
  queued slot to find the smallest domain regardless, and so can find
  the popped slot's position in that same pass -- push is O(1)
  (`push_back`), pop is O(1) (swap-with-last, `pop_back`), no insert-
  order maintenance at all. Ties broken explicitly by lowest slot id to
  match the original full-array scan's tie-break exactly, since the
  list's order is now insertion order, not slot-id order. Caught one real
  bug while implementing this: a second, inlined copy of the enqueue
  logic (the "same bookkeeping as enqueue()" fast path for a neighbor
  whose narrowed count is already known) updated `in_queue`/
  `queued_count` but not the new list, silently making any slot enqueued
  through that path invisible to the min-scan forever after -- caught
  immediately since it turned a 0.16s grid into an apparent hang, not a
  subtle wrongness. Verified byte-identical node/backtrack/restart counts
  on all three 30-grid real samples (seeds 42/7/99, zero mismatches) once
  fixed. Net effect: consistently ~2% faster in every sample (seed 42
  10.97s → 10.74s, seed 7 3.59s → 3.52s, seed 99 2.42s → 2.36s; ~17.0s →
  ~16.6s overall) with zero grids gained or lost in any of the three --
  a small but genuine, risk-free win, unlike the search-order-affecting
  changes above and below.

  **Cached neighbor length in `SlotCrossing`.** `Propagate`'s per-crossing
  loop -- its hottest loop, run on every popped slot -- re-derived
  `grid_.SlotById(sc.neighbor).length` on every single iteration, even
  though it's fixed for the life of the search. `sc.neighbor` can be any
  slot in the grid, so that lookup is a cache-miss-prone random access
  into a `vector<Slot>` (each `Slot` padded out by its own `cells`
  member), whereas `crossings_by_slot_[slot]` -- and so `sc` itself -- is
  already being scanned sequentially. Added a `neighbor_length` field to
  `SlotCrossing`, computed once in the constructor alongside the
  already-cached `crossing_id`, and read directly instead of re-deriving
  it. Purely an integer cached at construction (no floating point, unlike
  the crossing-weights change above), so provably behavior-preserving --
  confirmed byte-identical node/backtrack/restart counts on both several
  spot-checked grids and all three 30-grid real samples (seeds 42/7/99,
  zero mismatches). Net effect: consistently faster in every sample
  (seed 42 11.18s → 11.07s, -1.0%; seed 7 3.66s → 3.56s, -2.6%; seed 99
  2.49s → 2.39s, -4.0%; ~17.3s → ~17.0s overall, -1.7%) with zero grids
  gained or lost in any of the three -- another small, risk-free win in
  the same vein as the active-queue list just above.

  **Flat `slot_length_` array.** The same cache-miss-prone
  `grid_.SlotById(id).length` pattern the fix just above targets for
  crossing neighbors was also the way `SaveDomainOnce`, `Propagate`'s own
  popped slot, `SelectBranchSlot`, `Assign`, `Undo`, `Backtrack`, and
  `NogoodForbiddenWords` all looked up a slot's own length -- effectively
  once per node at minimum, across most of the hot call graph. Added
  `slot_length_`, a `vector<int>` indexed directly by slot id and
  populated once in the constructor, and replaced every one of those call
  sites with a direct read from it -- same reasoning as
  `SlotCrossing::neighbor_length`: a small, dense array is cheaper to
  keep in cache than a random access into `vector<Slot>`. Another
  integer-only, zero-floating-point change, so provably behavior-
  preserving; confirmed byte-identical node/backtrack/restart counts on
  six spot-checked grids and all three 30-grid samples (seeds 42/7/99,
  zero mismatches). Net effect, smaller than its two predecessors but
  still consistently non-negative in every sample (seed 42 11.07s →
  10.86s, -1.9%; seed 7 3.56s → 3.54s, -0.6%; seed 99 2.39s → 2.39s,
  ~flat; ~17.0s → ~16.8s overall, -1.4%), again with zero grids gained or
  lost anywhere -- diminishing but still real returns from this same
  class of fix, consistent with most of the grid's per-node work already
  having been wrung out by the changes above.

  **`Dictionary::LetterMask` moved into the header, in isolation.** An
  earlier session tried moving `LetterMask` along with five other trivial
  `Dictionary` accessors (`HasLength`, `NumWordsOfLength`, `WordsOfLength`,
  `FullDomain`, `ScoreOrder`) into the header all at once, for the same
  cross-TU-inlining reason `WordBitset`'s own methods already live there
  (see this project's earlier history) -- and found a small, consistent
  regression across a real sample, so it was reverted without ever
  isolating which of the six was responsible. `sample`-profiling a real
  hard grid (`grid_303.txt`) still showed `LetterMask` as a distinct,
  non-inlined symbol (~3% of samples) well after that revert, so this
  session retested it alone: `LetterMask` -- and only `LetterMask`, the
  one actually called from `Propagate`'s hottest inner loop, up to 26
  times per crossing -- moved into the header, the other five left in
  `dictionary.cpp`. Verified byte-identical node/backtrack/restart counts
  on six spot-checked grids and all three 30-grid real samples (seeds
  42/7/99, zero mismatches). Net effect: consistently faster in every
  sample this time (seed 42 10.86s → 10.67s, -1.8%; seed 7 3.54s →
  3.49s, -1.6%; seed 99 2.39s → 2.36s, -1.2%; ~16.8s → ~16.5s overall,
  -1.7%) with zero grids gained or lost anywhere -- confirming the
  earlier regression was caused by one or more of the other five
  functions, not `LetterMask` itself.

  Followed up by testing the two other candidates actually called from
  somewhere hot -- `WordsOfLength` (once per popped slot in `Propagate`,
  often several times per node) and `ScoreOrder` (once per node, in
  `Backtrack`'s candidate loop) -- moved into the header individually,
  each on top of the kept `LetterMask` change, each reverted alone
  (`NumWordsOfLength`/`HasLength`/`FullDomain` are cold-path-only --
  called at setup, never during search -- so not worth testing). Both
  showed the same small, consistent regression on a single 30-grid
  sample (seed 42): `WordsOfLength` 10.67s → 10.73s (+0.6%),
  `ScoreOrder` 10.67s → 10.75s (+0.7%), with the slowdown concentrated
  on the same handful of larger grids each time (`grid_053.txt`,
  `grid_058.txt`, `grid_288.txt`, `grid_328.txt`). Resolves the "which
  one(s)" question above without needing to test all five: `LetterMask`
  is the only one of the six called frequently enough (up to 26 times
  per crossing, vs. at most once per node for the other two hot
  candidates) to make the inlined-code-size tradeoff pay off.
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

  **Lazily-decayed crossing weights** (Eén & Sörensson's MiniSat, VSIDS-
  style activity bumping -- see `docs/bibliography.md`): `BumpCrossingWeight`
  used to decay *every* crossing's weight on *every* propagation failure --
  an O(total grid crossings) pass paid on most nodes of the search, not
  just the culprit crossing's own O(1) update. `sample`-profiling a real
  hard grid (`grid_303.txt`) showed `Propagate` dominating self-time as
  expected, but this pass was a real, previously-unexamined contributor
  buried inside it. `Solver::CrossingWeights` (`solver.hpp`) replaces the
  eager per-event decay of every weight with the same trick MiniSat uses
  for variable activity: track each crossing's weight as an unnormalized
  `offset` value against a single shared `scale` that absorbs the decay,
  so bumping one crossing touches only that one entry (O(1)) instead of
  the whole array, with an infrequent O(n) renormalization pass (roughly
  every ~2300 events) before `scale` could underflow.

  Algebraically exact, but **not floating-point-identical** to the eager
  version it replaces -- confirmed this is the eager float32 baseline's
  own accumulated rounding drift (switching the lazy version's internal
  arithmetic to double changed which grids diverged not at all), not a
  bug in the rewrite, but it means a tiny fraction of dom/wdeg's
  priority-sort ties land differently, which -- like any other change to
  per-node search behavior -- perturbs which restart's random seed ends
  up solving a given grid. Benchmarked accordingly, with the same rigor
  as a heuristic change (three independent 100-grid real samples, seeds
  42/7/99, `min_score=40`, 15s cap per grid), not treated as a provably
  safe refactor. Verified sound throughout (all 15 tests pass; newly-
  produced fills spot-checked for a preserved block pattern, every cell
  filled, no duplicate words). Net effect, restricted to grids solved
  either way in a given seed (so the comparison isn't skewed by a grid
  that timed out on one side): seed 42 (20 shared grids) 15.88s → 10.73s
  (-32%), seed 7 (24 shared grids) 4.27s → 3.53s (-17%), seed 99 (23
  shared grids) 1.65s → 2.38s (+45%, almost entirely `grid_128.txt`
  alone going 1.0s → 1.75s). Across all three, 67 shared grids: 21.79s
  → 16.65s, about 24% faster overall -- but seed 99 also *lost* one
  grid (`grid_307.txt`, 6.5s solved under the old code, 34s under the
  new one -- past the 15s cap, though still eventually solved and still
  correct) with no grid gained in exchange, anywhere. A real, mixed
  result in the same vein as nogood-recording-from-restarts above: net
  positive (and here, substantially so, on time) but not uniformly so,
  because a search-order-affecting change can't be otherwise on this
  solver's restart-heavy harder grids. Kept given the size of the net
  win relative to the one loss.
- **Narrow-domain candidate fast path, plus a real bug found while adding
  it.** `Backtrack`'s candidate loop (see above) tests `dict_.ScoreOrder(length)`
  membership one word at a time -- O(`NumWordsOfLength`) per branching
  node regardless of how narrow the domain actually is, fine near the
  root but wasteful once deep search has narrowed a slot to a handful of
  candidates out of a length group that can run into the thousands (short
  slots especially). Below a threshold, `Backtrack` now extracts just the
  live candidates (`WordBitset::AndNotAssign`, a new method, plus
  `AppendSetBits`) and sorts that small set by a precomputed
  `Dictionary::ScoreRank` (the inverse of `ScoreOrder`, built once at load
  time) instead of walking the whole length group -- same "direct
  extraction below a threshold" shape as Propagate's
  `kDirectLookupThreshold` split, applied to word selection instead of
  letter viability.

  Two mistakes surfaced building this, both worth recording:

  1. **A real, pre-existing correctness bug.** `NogoodForbiddenWords`
     returns a pointer into per-length *scratch* state
     (`nogood_forbidden_scratch_by_length_`), reused across calls. The
     original code held that pointer across `Backtrack`'s whole candidate
     loop -- which recurses back into `Backtrack` -- so a descendant call
     for a *different* slot of the *same length* would silently overwrite
     the very buffer an ancestor frame's loop was still reading from on
     its next iteration. Harmless on a search's very first attempt (no
     nogoods exist yet), but from the second attempt on, this could make
     a genuinely-exhaustive attempt wrongly skip a valid candidate --
     the specific failure mode being `Solve()` reporting a satisfiable
     grid as UNSAT. Fixed by merging any nogood-forbidden words into a
     local `WordBitset` copy immediately (`effective_used`, folded into
     the existing `used` mask) instead of holding a pointer across the
     recursive loop -- the same fix shape applied to the new narrow-path
     candidate list itself (a per-frame `std::vector`, not scratch, for
     the same reason). Verified via
     AddressSanitizer + UndefinedBehaviorSanitizer on a real restart-heavy
     grid (`grid_053.txt`, which racks up 13+ nogood-triggering restarts
     under `min_score=40`) -- clean, and the resulting fill still passes
     block-pattern/no-duplicate validation. No dedicated unit test added
     (reliably forcing two same-length slots into a nogood-aliasing
     collision deterministically in a small synthetic grid is fragile);
     the sanitizer run against a real grid that's known to exercise this
     exact path stands in for one.
  2. **A self-inflicted regression, caught by benchmarking before
     shipping.** The first version of this fast path computed
     `domain_count` via a fresh `WordBitset::CountAndNot` at the top of
     *every* `Backtrack` call, to decide which path to take -- paid even
     when the answer was "take the plain scan," which is most nodes.
     `SelectBranchSlot` already computes this exact value for the chosen
     slot while scoring every candidate in the component; the fix was to
     have it return that count (`out_domain_count`, an output parameter)
     instead of recomputing it. Caught because seed-42 benchmarking
     showed a net *regression* (total time 2.44s → 3.42s) despite
     byte-identical node counts -- the fast path itself wasn't the
     problem, the redundant popcount pass paid by *every other* node was.

  A third thing this surfaced, unrelated to the bug fix: `SolveParallel`
  benchmark comparisons are inherently noisy in a way single-threaded
  ones aren't -- which worker's random attempt happens to finish first is
  real-time-scheduling-dependent, so two runs of *byte-identical* code can
  report different node counts for the same grid (confirmed directly:
  several grids' node counts differed between two back-to-back runs of
  the *same* binary). `benchmarks/bench_subset.py` gained a `--threads`
  flag (forwarded to `xfill_cli`'s 4th argument) so a change like this one
  can be isolated from that noise via `--threads 1`, matching
  `SolveParallel`'s own worker-0 sequence exactly.

  Measured effect (single-threaded, `--threads 1`, isolating this change
  from `SolveParallel`'s scheduling noise above; three seeds, `min_score=40`,
  15s cap): node counts identical to the pre-change baseline on every
  grid in all three seeds (confirms this is a pure constant-factor change,
  not a search-order change) with total solved-time down 1.15%/1.75%/2.1%
  → -3.01%/-1.2%/-2.1% once the `SelectBranchSlot` reuse fix (above)
  landed. `kCandidateDirectThreshold = 1000` was picked the same way as
  Propagate's threshold: re-running the benchmark at 200/1000/4000 and
  taking the plateau (-1.2%/-3.0%/-2.9%, seed 42). Modest, but real,
  uniform (no grid regressed in any seed), and free of any search-order
  risk -- kept alongside the bug fix above, which is not optional
  regardless of the speed number.
- **Restarts.** Geometric backtrack-budget growth
  (`kInitialBacktrackLimit = 500`, `kRetryGrowthFactor = 1.1`), motivated
  by Gomes, Selman & Kautz's heavy-tailed-runtime-distribution result.
  The first attempt is fully deterministic; restarts pick their branch
  slot via weighted-random choice among the best few dom/wdeg-ranked
  slots (`kRandomSlotWeights = {4, 2, 1}`). Crossing weights carry over
  across restarts; the search tree does not. Because the budget only
  grows, the search stays complete.

  **Nogood recording from restarts** (Lecoutre, Sais, Tabary & Vidal,
  IJCAI 2007 — see `docs/bibliography.md`): whenever a slot's candidate
  loop runs to genuine, complete exhaustion (every candidate tried and
  undone with the attempt not yet aborted) and that specific exhaustion
  is what pushes the current attempt over its backtrack budget, the
  entire current assignment is recorded as a nogood — a combination
  proven, by that exhaustive search, to never lead to a solution.
  Future restarts within the same `Solve()` call check, once per node,
  whether assigning the branch slot to a given word would complete any
  recorded nogood (all its *other* pairs already matching the current
  assignment); if so, that word is skipped without re-deriving the
  failure. This is a structurally different mechanism from the plain
  nogood learning tried and reverted in an earlier session (see
  `docs/bibliography.md`'s "Consulted for context" section) — recording
  only from restart-triggering, fully-exhausted branches keeps the
  nogood count bounded by the restart count and avoids that earlier
  attempt's failure mode (pruning that changes how much work the *same*
  attempt does before hitting its own budget).

  **Measured effect — real, but modest and mixed, unlike the changes
  above.** Verified sound (all 15 tests pass, including the
  no-solution-exists cases; newly-solved grids' outputs checked
  independently for a preserved block pattern, every cell filled, and no
  duplicate words) and tested across three independent 100-grid real
  samples (seeds 42, 7, 99) against a clean pre-nogood build, each run
  twice to rule out measurement noise (a first pass showed one grid's
  time swing by 15x between runs with nothing else changed — a reminder
  that single-run timings on this machine aren't trustworthy for a
  change this size, and both runs of everything reported here were
  reproduced before being written down). Net effect: seed 42 gained one
  previously-timing-out grid (`grid_479.txt`) with no losses, at a ~6%
  time cost on the 78 grids solved either way; seed 99 similarly gained
  one (`grid_472.txt`) with no losses and only ~1.6% overhead; seed 7,
  however, *lost* one previously-solved grid (`grid_453.txt`, 12.9s →
  timeout) with no gains, and ran ~9% slower overall on the 83 grids
  solved either way. Net across all three: two grids gained, one lost —
  a real but modest improvement in solvability, not a clear win the way
  the constant-factor fixes above were. The mixed, per-grid result is
  the expected signature of this technique (and the reason the earlier
  plain nogood-learning and backjumping attempts were reverted): any
  change to per-node search behavior perturbs which restart's random
  seed ends up solving a given grid, helping some and hurting others,
  even when the pruning itself is sound. Kept anyway, since the net
  effect across all three samples is positive and the technique
  directly targets this project's harder, restart-heavy grids rather
  than the already-fast ones.
- **Parallel restarts (`Solver::SolveParallel`).** Every technique above
  optimizes a single search thread; this one instead runs several of
  them at once. The theoretical basis was already cited in this project
  (Gomes, Selman & Kautz, above) but never acted on: if restart helps
  because backtracking runtime is heavy-tailed and a *different* random
  run of the same search often finishes fast even when this one hasn't,
  then running several different random runs *simultaneously* across
  hardware threads should shorten wall-clock time to the first lucky one
  by roughly the thread count, on grids where that's the bottleneck --
  this machine has 14 cores and the solver had used exactly one of them,
  throughout its whole history, until now.

  Implemented as a portfolio, not a shared-state parallel search: each
  worker gets its own private `Solver` instance (own domains, trail,
  crossing weights, nogoods, RNG -- nothing search-related is shared
  across threads), so there is no synchronization anywhere inside
  `Propagate`/`Backtrack`/`Assign`/`Undo`, only a single
  `std::atomic<bool>` cancellation flag checked once per node (in
  `Backtrack`, same cadence as the existing `aborted_` check) so every
  other worker unwinds within a node of whichever one finds a solution
  first. Worker 0 is seeded to reproduce today's single-threaded sequence
  exactly (attempt 0 deterministic, then randomized restarts); every
  other worker gets a distinct, non-overlapping attempt-number range so
  its own attempt 0 is *already* randomized -- otherwise it would just
  redo worker 0's identical deterministic pass for free, wasting an
  entire thread. Still complete: every worker's cancellation flag check
  only ever short-circuits its *own* search, never invents a result, so
  any worker that returns "no solution" without itself having been
  cancelled has done so via a genuine, exhaustive search from the same
  root-propagated domains every worker starts from -- reducing directly
  to `Solve()`'s own already-established completeness. (Originally this
  meant waiting for literally every worker to reach that state before
  reporting UNSAT; see "Tried and kept: unlimited_budget in
  SolveParallel" below for why that changed to cancelling the rest as
  soon as *any one* worker gets there.)

  Verified sound before measuring speed at all: the existing 15-test
  suite continues to pass through `Solve()`'s unchanged default
  behavior (the new `attempt_offset`/`cancel` parameters both default to
  values that reproduce it exactly), plus four new tests exercising
  `SolveParallel` directly (a 1-thread run matching `Solve()` verbatim, a
  multi-thread run against a small satisfiable grid, an
  every-worker-proves-UNSAT case, and the `num_threads=0` auto-detect
  path). More importantly for a concurrency change, the full test suite
  and several real 15x15 solves were run under ThreadSanitizer -- zero
  data races reported anywhere, consistent with the "nothing shared but
  one atomic flag" design.

  **Measured effect: a real, large net win, but not a uniform one.**
  Three independent 30-grid real samples (seeds 42/7/99, `min_score=40`,
  15s cap, comparing against the single-threaded baseline immediately
  before this change), using `hardware_concurrency()` threads (14 on the
  machine this was benchmarked on): total time across the 67 grids
  solved either way dropped from 16.51s to 9.31s (-43.6%, ~1.8x faster),
  and two previously-timing-out grids newly solved (`grid_047.txt`,
  `grid_481.txt`) with zero grids lost. Individual grids varied hugely:
  `grid_053.txt` went 7.07s → 0.44s (15.5x), `grid_360.txt` 0.46s →
  0.005s (99% faster) -- but a handful of grids got *slower*, some
  substantially: `grid_424.txt` 1.74s → 3.36s, `grid_128.txt` 1.74s →
  2.85s, `grid_328.txt` 0.83s → 1.52s. Root-caused by direct
  measurement, not guessed: on these specific grids, worker 0's plain
  deterministic search (or an early low-offset restart) was already
  close to the fastest available path, and adding more concurrent
  workers only adds CPU/cache/memory-bandwidth contention with no
  compensating benefit, since nothing else finds a luckier path fast
  enough to matter -- confirmed by rerunning `grid_328.txt` at 2/4/8/14
  threads and seeing time increase monotonically with thread count
  (0.85s/1.00s/1.25s/1.52s). This is the expected, inherent signature of
  portfolio parallelism, not a bug: it helps most exactly on the grids
  that need restart's "heavy tail" escape hatch, and can only add
  overhead on ones that didn't need it. A large number of already-fast
  grids also show a fixed few-millisecond overhead from spawning and
  constructing `hardware_concurrency()` `Solver` instances regardless of
  whether the grid needs them, imperceptible in absolute terms (a 2ms
  solve becoming 5ms) but visible as a large *percentage* change on a
  tiny baseline. Kept given the size of the net win and that it directly
  targets this project's worst cases (the restart-heavy grids that
  dominate real-world timeouts) rather than trading away performance on
  already-fast ones to get it -- `xfill_cli` now defaults to
  `SolveParallel`; pass an explicit `num_threads` of 1 for the old
  single-threaded behavior (useful for reproducible timing comparisons,
  as every non-parallel benchmark elsewhere in this document was).

  **Tried and reverted: a "head start" for worker 0.** Since the
  contention regressions above are all cases where worker 0 alone was
  already close to fastest, the obvious-looking fix is to give worker 0
  up to 200ms uncontended (polled every 2ms, ended early on either a
  solution or a proven-UNSAT result) before spawning the rest at all.
  Implemented, made correct (a `worker0_done` flag distinct from the
  existing solution-only `cancel` flag, so a fast *UNSAT* also ends the
  head start early instead of always sitting out the full 200ms), and
  passed all 20 tests plus a ThreadSanitizer pass -- but benchmarking
  (seed 42, 30-grid sample) showed a net *regression*: total time across
  the 20 grids solved both ways went 2.11s -> 2.85s (+35%). It did help
  the grid it targeted (`grid_328.txt`: 1.517s -> 1.171s), but it hurt
  far more grids than it helped: several already-fast grids that
  previously got solved almost instantly by a lucky *non-zero-offset*
  worker -- not by needing a restart, just by that worker's own already-
  randomized attempt 0 stumbling onto a solution faster than worker 0's
  deterministic path -- now have to wait out (most of) the head start
  before those workers are even allowed to start, e.g. `grid_013.txt`
  0.009s -> 0.160s, `grid_058.txt` 0.041s -> 0.252s, `grid_360.txt`
  0.005s -> 0.206s. In other words, the "extra workers only add
  contention" assumption behind the head start only holds for the small
  set of grids identified above; for most grids in the sample, the extra
  workers' *own* randomized starting points are themselves a source of
  speed, not just a restart-escape mechanism, so delaying them costs more
  than the contention they'd otherwise cause. Reverted before running the
  remaining two seeds -- the seed-42 signal was already large and
  directionally consistent with the mechanism, not close enough to be
  worth chasing with retuning (e.g. a shorter head start would just
  shrink both effects, not resolve the tension between them).

  **Tried and reverted: adaptive search-space partitioning.** Prompted by
  a head-to-head benchmark against two other crossword-fill engines this
  project has drawn on (`rf-/ingrid_core`, `rainjacket/orca-solver` --
  see docs/bibliography.md; neither's code or repo history was copied
  into this project, only build-from-source comparisons run outside it):
  orca-solver's 14-way parallel mode solved two grids (`grid_120.txt`,
  `grid_303.txt`) from the real 30-grid sample that `SolveParallel`
  couldn't touch even single-threaded, though at a real cost of its own
  -- orca's parallel numbers otherwise cluster near a ~3-second floor
  even on grids solved in milliseconds elsewhere, an artifact of its
  default 3-second partition-split-timeout, not real search cost. Orca's
  parallelism divides the search space itself across threads
  (partition-based) rather than racing independent full-space random
  restarts the way `SolveParallel` does -- a genuinely different lever:
  restarts help when one worker's own bad luck is the problem (a
  *different* seed sails through), partitioning helps when every seed
  struggles about equally because the space itself is wide, not because
  any one of them got unlucky.

  First tried as a cheap upfront heuristic -- guess from grid structure,
  before searching at all, whether a grid needs partitioning. Checked
  three linear-time candidate signals (the root branch slot's domain size
  after propagation, total domain size summed across all slots, grid
  open/blocked cell ratio) against the two grids known to need
  partitioning versus two known to be fine with restarts alone
  (`grid_053.txt`, `grid_328.txt`) versus trivially-easy ones. None
  separated the categories at all -- e.g. `grid_013.txt` (solves in
  milliseconds) and `grid_120.txt` (needs partitioning) have the *exact
  same* open-cell ratio, and `grid_048.txt`/`grid_303.txt` share the
  identical root-domain value despite opposite categories. Real published
  15x15 grids are all built to roughly the same density (house style for
  a publishable crossword), so grid-level structural stats barely vary
  across the whole corpus -- consistent with what this project already
  cites from Anbulagan & Botea: instance hardness is intrinsic to specific
  letter-pattern interactions, not predictable from surface structure.

  Rebuilt around an *adaptive* signal instead: a handful of scout workers
  (`kScoutCount = 4`) run ordinary unrestricted restart-portfolio search
  first; after a short grace period (`kGracePeriod = 150ms`), if their
  backtrack counts (a new lock-free `std::atomic<uint64_t>` mirror,
  `BacktracksSoFar()`, polled without any lock -- restart counts were
  tried first and rejected, since a restart only completes every ~500+
  backtracks, too coarse to read anything within one grace period) are
  close together rather than spread out, the remaining workers switch to
  partitioning (`PartitionSpec`: worker `i` of `count` gets a contiguous
  slice, in score order, of whichever slot dom/wdeg would branch on
  first -- that first decision is always deterministic, computed
  independently and identically by every worker). This classifier
  actually worked, cleanly and reproducibly (verified across repeated
  runs): `grid_120.txt`/`grid_303.txt` triggered partitioning with a
  backtrack spread of 22-25% of the max; `grid_053.txt`/`grid_328.txt`
  correctly did not, at 41% spread -- a real, stable gap between the two
  categories that the earlier static signals never showed.

  Implemented soundly (24/24 tests including new `PartitionSpec`-specific
  cases, clean under ASan/UBSan and ThreadSanitizer including a real run
  against a restart-heavy grid at 14 threads) -- but the actual *remedy*
  didn't reproduce orca's benefit. Benchmarking (seed 42, 30-grid sample)
  showed both a small broad regression and a large targeted one:
  total time across the 20 grids solved both ways went 1.80s -> 1.95s
  (+8%), and `grid_120.txt` specifically -- the grid this was built to
  fix -- went from timing out at 15s under plain `SolveParallel` to a
  reproducible 41.6s and 39.1s across two runs once actually given
  enough time to finish, worse than plain `SolveParallel`'s own 19.8s on
  the same grid, same budget. Two distinct costs, both traced directly
  rather than guessed: (1) the grace-period wait is a real, unconditional
  latency floor -- the orchestrator thread only notices a scout already
  solved the grid at its next 5ms poll tick, so even trivially-fast grids
  now pay a small fixed tax, visible as a near-uniform +4-5ms across many
  already-millisecond grids in the per-grid diff; (2) on grids that
  *don't* decide to partition (the common case), the non-scout workers
  still don't start until the full grace period elapses -- a structural
  echo of the head-start mistake just above, costing real time by
  delaying workers that might have been the lucky ones, for zero benefit
  once the decision comes back "no." Whether restricting only the *root*
  decision is simply too shallow a cut to matter when a grid's difficulty
  lives many levels deeper in the tree, or orca's own dynamic
  re-partitioning (deliberately not ported here, being the source of its
  3-second floor artifact noted above) is doing something this static
  one-time slice can't, is still an open question;
  reverted rather than pursued further, since the evidence needed to
  justify novel search-space-partitioning is different in kind from the
  bookkeeping/constant-factor tuning most of this document covers, and it
  wasn't there.

  **Tried and kept: word-choice randomization on restarts.** A follow-up
  to the partitioning attempt above, prompted by a direct benchmark
  against `orca` on the specific grid John Hawksley (orca's author) used
  to demonstrate it: a 7x7 with two seed letters and no black squares at
  all. `orca` solved it single-threaded in 201.7s; the unmodified solver
  didn't finish in any tested budget. Root cause, confirmed by direct
  instrumentation rather than guessed: `Backtrack`'s word-candidate order
  is plain `dict_.ScoreOrder` best-first, and restarts (see the "Restarts"
  section above) only randomize *slot* choice, never word choice, even
  though `randomize_slot_choice_` was available to gate on. On a grid this
  wide open, a slot's live domain routinely holds thousands of candidates
  (every word length in this dictionary has well over 1000 entries — see
  the dictionary tuning table), and the true solution isn't necessarily
  built from top-score words at every slot. Strict best-first order meant
  *every* restart re-tried the same top-ranked words before ever reaching
  whatever word the real solution needed at that slot — slot-choice
  diversity alone couldn't route around that, since word order at any
  given slot was identical across all restarts regardless of how many
  times the search retried.

  First fix (shuffle candidates whenever `randomize_slot_choice_` is true,
  no further gating) found the grid's actual solution single-threaded in
  166.7s and 14-threaded in 63.0s — both real wins over orca's 201.7s. But
  it regressed the existing 20-grid corpus sample: `grid_328.txt` went
  from solved in 0.83s to a 20s timeout. Diagnosed directly: every word
  length in this dictionary has well over 1000 candidates (see the
  dictionary tuning table above), so a slot's very *first* branch in a
  component routinely starts in `Backtrack`'s large-domain candidate
  branch on *any* grid, not just a wide-open one — gating purely on that
  branch (as opposed to `kCandidateDirectThreshold`'s small-domain branch)
  still shuffled on essentially every restart of every grid. A second
  attempt gated on restart *count* instead — only shuffle once at least
  `kWordShuffleRestartThreshold` (20) restarts have already happened —
  fixed the corpus regression cleanly (all 20 grids' nodes/backtracks/
  restarts came back byte-identical to the unmodified solver) but
  introduced a new problem specific to `SolveParallel`: the threshold
  is per-worker, so every one of the 14 workers had to independently burn
  through the same ~20-restart, guaranteed-unproductive warm-up before
  *any* of them could get lucky via shuffling — wasting exactly the
  parallelism that matters most for a grid like this. Fixed by making the
  gate worker-aware: a `SolveParallel` worker with a nonzero
  `attempt_offset_` is already diversifying from its own local attempt 0
  by design (see the "Restarts" section's `global_attempt` discussion), so
  it skips the restart-count gate and shuffles immediately; only worker 0
  (or a plain single-threaded `Solve()` call, i.e. `attempt_offset_ == 0`)
  keeps the restart-count gate, since that exact sequence is what the
  corpus regression check validated byte-for-byte.

  Final verification, run with no other CPU-competing processes on the
  benchmark machine (an earlier round of timings was inflated by a
  forgotten background process left over from an unrelated experiment —
  see the "Benchmarking philosophy" section's isolation note): regression
  suite unchanged (12/20 solved, identical nodes/backtracks/restarts on
  every grid, single-threaded). The honest picture on the Hawksley 7x7
  grid itself, after many repeated runs (see "Tried and reverted:
  cell-level branching for large domains" just below for the full
  investigation this triggered): this fix *does* let the grid finish at
  all, and produced two clean, real wins over orca's 201.7s (166.7s
  single-threaded, 63.0s 14-threaded) -- a categorical improvement over
  the unmodified solver, which never finished this grid in any tested
  budget. But it is not a guaranteed sub-5-minute result on every run:
  repeated single-threaded and 14-threaded trials on this same grid also
  took several times longer than that, sometimes exceeding it, purely
  from which random shuffle a given run happened to draw. This is the
  expected signature of restart-based search's heavy-tailed runtime
  (Gomes, Selman & Kautz -- see the "Restarts" section above and "Known
  hard cases" below): a fix that makes the *typical* case fast doesn't
  make every draw fast, and no amount of gating/threshold-tuning changes
  that without changing the underlying algorithm. Kept regardless, since
  "sometimes fast, previously never" is still strictly better than
  "never," and it costs the existing benchmark corpus nothing.

  **Tried and reverted: cell-level branching for large domains.**
  Prompted directly by the honest limitation just above: the user asked
  for the Hawksley grid to solve in under 5 minutes *reliably*, and
  word-choice randomization alone doesn't guarantee that. The
  architectural candidate for a real fix, flagged since this document's
  "Future work" section and orca's own headline difference (see
  docs/bibliography.md): branch on one *letter* at a time (a specific
  cell) instead of enumerating whole words, cutting the branching factor
  at any one large-domain node from thousands down to at most 26, and
  reusing `Propagate`'s existing crossing-cascade machinery completely
  unchanged (it already narrows domains of any size, not just
  singletons). Implemented as a new `SelectBranchOffset` (picks the
  slot's most letter-constrained position; a first cut scored purely
  against the branch slot's own domain, a second cut against the *joint*
  viable-letter set with the crossing neighbor's domain too -- a real
  two-sided MRV) plus a per-letter branch-and-propagate loop replacing
  `Backtrack`'s large-domain word-enumeration branches entirely. Verified
  sound first: 29/29 tests (plus new cases exercising the large-domain
  path directly), clean under ASan/UBSan/TSan including a real
  multi-threaded solve.

  Measured effect was a real, unambiguous *split* result. On the general
  15x15 benchmark corpus, a clean, broad win: 12/20 solved (same set, zero
  losses) but total time across the solved grids dropped from ~14.8s to
  6.89s single-threaded (`grid_053.txt` alone: 12.463s -> 4.770s), and
  default parallel mode improved from 12 to 13 solved (`grid_115.txt`
  newly solved) with no losses either. But on
  the actual motivating grid -- Hawksley's 7x7 -- it did not reproduce the
  win it was built for. Four independent, controlled comparisons all
  agreed: (1) `unlimited_budget` (a single continuous DFS, no restarts) at
  a fixed deterministic seed reached only ~4M backtracks in 115s without
  solving, versus word-level+shuffle's earlier 166.7s full solve; (2)
  adding the joint two-sided MRV refinement to `SelectBranchOffset` barely
  changed that trajectory -- the grid is symmetric enough early in search
  that neither one-sided nor joint scoring had much signal to exploit;
  (3) a randomized `unlimited_budget` run (matching exactly what
  `SolveParallel`'s dedicated worker actually does) reached 320K backtracks
  in 280s, still unsolved; (4) a controlled, apples-to-apples restart-based
  comparison -- word-level+shuffle vs. cell-level branching, both
  single-threaded, both started at the same moment on an otherwise-idle
  machine -- showed word-level consistently completing more restarts per
  unit wall-clock time throughout. A final clean 14-thread run of the
  cell-level version (no other processes competing, a fresh 5-minute
  budget) still hadn't solved when stopped. Root cause, reasoned through
  rather than fully profiled given time spent: cell-level branching's
  finer granularity means many more node visits are needed to reach the
  same assignment depth than word-level branching's "one decision, one
  whole dictionary word" (a real, actually-occurring string, not an
  arbitrary letter sequence) -- and on a grid this symmetric and this
  weakly constrained, that added node count wasn't offset by the smaller
  per-node branching factor the way it was on the more typically-
  constrained 15x15 corpus, where propagation narrows domains down to the
  small-domain branch much sooner. Reverted in favor of word-level
  shuffling, since meeting the concrete, explicitly-requested goal (this
  grid, reliably fast) mattered more here than the broader (but here
  counterproductive) architectural change -- `SelectBranchOffset` and its
  supporting `WordBitset::CountAnd` were removed along with it, rather
  than left as unused dead code. Still the more principled fix for this
  class of grid in the abstract (see "Future work" below); if pursued
  again, the corpus win suggests it's worth keeping for *some* regime, so
  a hybrid (cell-level only for the very largest domains, or only for the
  first few decisions of a component) is the more promising next attempt,
  not a wholesale replacement.

  **Tried and kept: word-choice randomization extended to the small-domain
  branch.** The actual root cause of the whole cell-level-branching
  detour above, found by direct instrumentation rather than guessed:
  every one of five different large-domain branching heuristics tried on
  the Hawksley 7x7 grid (letter-count MRV, joint two-sided MRV, true
  global cell MRV, a faithful replica of orca's own work-estimate
  heuristic read directly from its Rust source, and a reversed-scan-order
  variant) produced numerically *identical* node/backtrack progressions.
  A debug counter placed directly in `Backtrack`, tracking how often
  `domain_count` actually exceeds `kCandidateDirectThreshold` versus
  staying at or below it, showed the large-domain branch firing **zero**
  times across 17,000+ real search nodes on this grid: every single
  branch decision, for the entire search, falls at or below the
  threshold (the very first one at domain_count=254, likely because the
  H/T seed letters and the resulting crossing cascade narrow every slot's
  domain well below 1000 within the first couple of assignments, even
  though the grid is otherwise "wide open"). All five heuristic variants
  were dead code for this specific grid+dictionary combination the entire
  time -- which is exactly why they were indistinguishable.

  Reading orca's actual source (`crates/solver/src/search.rs`,
  `constraint.rs` -- consulted for reference only, never copied into this
  project or its git history, per this session's standing constraint) is
  what surfaced this: orca's `find_best_crossing` scores candidates by
  Σ(count_a\[letter\] × count_b\[letter\]), a real subtree-size estimate,
  over a *bounded* scan of at most 15 crossings -- a materially different
  (and, on reflection, more sophisticated) metric than the plain
  letter-count MRV this project had been trying. Replicating it faithfully
  (including narrowing only the smaller-domain side directly and letting
  `Propagate`'s existing cascade handle the other, and orca's surprising
  choice of *no* value-ordering heuristic at all -- plain ascending letter
  order, relying entirely on branch selection) still showed the identical
  zero-large-domain-hits result on this grid, which is what finally forced
  the instrumentation that found the real explanation above, rather than
  continuing to guess at heuristic refinements.

  With the real bottleneck identified -- the *small*-domain branch
  (`domain_count <= kCandidateDirectThreshold`), which had used a plain,
  unconditional `ScoreRank`-sorted order since long before this session,
  completely unaffected by `shuffle_words`/`kWordShuffleRestartThreshold`
  -- the fix was small: gate that branch's ordering on `shuffle_words`
  too, sorting when false and shuffling when true, exactly mirroring the
  large-domain branch's own gate. The same protective reasoning applies
  unchanged: `grid_328.txt` and the rest of the corpus solve within a
  couple dozen restarts at most, so `shuffle_words` (and thus this new
  branch) never activates for them regardless of the domain regime they
  actually live in.

  Verified sound first: 29/29 tests, clean under ASan/UBSan/TSan.
  Regression-checked against the same 20-grid sample: single-threaded
  byte-identical to the pre-existing baseline (12/20, matching every
  node/backtrack/restart count exactly -- confirms the gate protects the
  corpus regardless of which branch a grid's domains happen to live in);
  default parallel mode reached **14/20 solved**, this project's best
  result yet on this sample -- `grid_303.txt` newly solved (never solved
  in *any* configuration tried this session before now), `grid_045.txt`
  newly solved, `grid_053.txt`/`grid_058.txt` substantially faster.
  `grid_328.txt` shows the same familiar tradeoff pattern as other broad
  wins in this document (0.83s -> 3.65s) and `grid_115.txt` remains a
  timeout (already understood, see the crossing-weight-sharing entry
  above). On the actual motivating grid: a 14-threaded run solved in
  224.7s -- not quite under orca's 201.7s on this specific sample, but a
  real, working solve via a code path that actually executes for this
  grid, unlike every cell-level-branching variant tried before it, none
  of which ever solved it in any tested window. Given this search's
  well-established heavy-tailed variance (see "Known hard cases" and the
  word-choice-randomization entry above), no single sample -- on either
  side of orca's number -- should be read as the final word; this is
  kept because it is a genuine, validated improvement to the actual
  code path this grid uses, not because one sample beat one other
  sample.

  **Follow-up: oversubscribing `SolveParallel`'s thread count.** With the
  fix above actually exercising the branch this grid's search lives in,
  a natural question: does racing *more* independent restart-portfolio
  workers than this machine's 14 physical cores help further? Every
  worker past the first two (worker 0, and the dedicated
  `unlimited_budget` one) already skips `kWordShuffleRestartThreshold`'s
  gate entirely (`attempt_offset_ > 0`, see that member's comment in
  solver.hpp) and shuffles from its own attempt 0 -- so more of them
  running concurrently means more independent, already-diversifying
  lottery tickets racing at once, exactly the lever Gomes/Selman/Kautz's
  heavy-tailed-runtime argument predicts should help (see the "Restarts"
  section above). Tested directly on the Hawksley grid, no code change
  (`num_threads` is already a plain `SolveParallel` argument): 28 threads
  -> 142.3s and 104.6s across two runs, 42 threads -> 85.6s, 56 threads
  -> 112.4s -- every oversubscribed run beat orca's 201.7s, several by a
  wide margin, with 42 threads (3x this machine's 14 physical cores)
  looking like the sweet spot before contention overhead starts eating
  into the gain (56 threads' 112.4s being slower than 42 threads' 85.6s).
  Not committed as a new default -- `xfill_cli`'s `num_threads=0` still
  means `hardware_concurrency()` (14 here), matching this project's
  general-purpose usage -- but confirms that for a specific hard grid
  known in advance, passing an oversubscribed thread count is a real,
  measured lever, not just a hopeful guess.

  **Tried and kept: `unlimited_budget` in `SolveParallel`.** A separate,
  earlier finding in this same investigation: `grid_072.txt` and
  `grid_217.txt` (real, scraped, actually unsatisfiable at
  `min_score=40`) never resolved -- not solved, not proven UNSAT --
  after 90 minutes of nothing but restart-based search, single- or
  14-threaded. Root cause is inherent to restarts, not a bug: each
  restart discards almost all of the previous attempt's progress toward
  a genuinely exhaustive pass, so a search that's *supposed* to
  eventually cover the whole space in the limit can in practice restart-
  thrash forever without ever actually finishing one. `Solve()` already
  had `unlimited_budget` (a single continuous DFS, no restarts at all,
  guaranteed eventually complete either way -- see its doc comment) but
  it wasn't reachable from `SolveParallel`/`xfill_cli`. Wired in by
  dedicating exactly one worker (the last one, whenever `num_threads >
  1`) to `unlimited_budget = true`, leaving the rest as the existing
  restart portfolio -- the same "portfolio, no classifier" shape as
  `SolveParallel` itself, rather than trying to detect upfront which
  grids need it (this project's adaptive-partitioning attempt above
  already showed that kind of detection is its own source of cost and
  risk).

  Doing this exposed a real latent inefficiency in `SolveParallel`
  worth fixing at the same time: previously, a worker's genuine (non-
  cancelled) "no solution" return didn't cancel the others -- the call
  waited for literally every worker to independently reach that state,
  even though any *one* of them reaching it, restart-based or
  `unlimited_budget`, already constitutes a sound, complete proof (see
  the "Parallel restarts" section above). Fixed by reusing the existing
  `cancel` atomic for this case too: a worker's nullopt that wasn't
  itself caused by cancellation now cancels every other worker via the
  same compare-and-swap the solution-found path already used. Verified
  directly: `grid_072.txt`'s standalone `unlimited_budget` validation
  (no restarts, run in isolation outside `SolveParallel`) reached a
  genuine, exhaustive "no solution" proof in 84 minutes
  (nodes=53859939, backtracks=1890357) -- something no purely
  restart-based configuration achieved in 90 minutes during the earlier
  three-way benchmark against `ingrid_core`/`orca` (see
  docs/bibliography.md). `grid_217.txt`'s equivalent validation run was
  still in progress (113M nodes, 7.2M backtracks after 2h19m, no
  conclusion either way) when it was stopped to free the benchmark
  machine for the Hawksley 7x7 investigation above -- inconclusive, not
  negative; worth relaunching if grid_217.txt specifically matters
  again. Regression-checked against the same 20-grid sample in default
  (parallel) mode: 13/20 solved (up from 12 -- `grid_115.txt` newly
  solved within the 20s cap, no grid lost), consistent with the new
  cross-worker cancellation only ever letting a call finish sooner, never
  later. Verified under ThreadSanitizer, including a real multi-threaded
  solve, not just the unit suite -- no data races reported, consistent
  with the change reusing the existing single-atomic-flag design rather
  than adding new shared state.

  **Tried and kept: crossing weights shared across `SolveParallel`
  workers.** After word-choice randomization, `unlimited_budget`, and a
  reverted cell-level-branching detour all failed to make the Hawksley
  7x7 grid *reliably* fast (see above -- restart-based search's
  heavy-tailed runtime is real and doesn't fully go away by tuning), the
  next lever tried: each `SolveParallel` worker keeps its own private
  `CrossingWeights` (dom/wdeg's per-crossing "how troublesome has this
  been" learning, see the class comment above), so N independent workers
  restarting independently never share what any one of them discovers
  about which crossings keep wiping out -- even though the underlying
  grid/dictionary structure a crossing is troublesome *because of* is the
  same for all of them.

  Added a second, much simpler `SharedCrossingWeights` alongside (never
  instead of) the existing per-worker one: a plain `std::atomic<uint32_t>`
  wipeout counter per crossing, bumped (relaxed fetch_add) by every
  worker on every wipeout and read (relaxed load) by every worker's
  `SlotWeight`. Deliberately not the same lazy-decay scheme as the
  per-worker version: that scheme's shared `scale` divisor makes every
  `Bump()` depend on the exact value of every prior one, which is only
  race-free with a single writer -- a concurrently-written signal needs
  each crossing's update independent of every other crossing's and of
  order, which a plain atomic increment already is. Also drops
  recency-weighting for this signal specifically: it aggregates the whole
  portfolio's cumulative experience over one bounded run rather than one
  search's own evolving trajectory, so decay matters less here than for
  the per-worker signal.

  Two correctness fixes needed before the numbers meant anything.
  First: `num_threads=1` calls wire this up unconditionally -- discovered
  because it silently broke `bench_subset.py --threads 1`'s
  reproducibility guarantee (a lone worker got a real, if redundant,
  `SharedCrossingWeights` pointer, perturbing `SlotWeight`'s sum with an
  extra `+get(id)-1.0f` term even with only one writer, changing branch
  order and so node counts for a call this project's whole benchmarking
  methodology assumes is byte-identical run to run). Fixed by gating on
  `num_threads > 1`, same condition `unlimited_budget`'s own worker
  dedication already uses. Second: the dedicated `unlimited_budget`
  worker's entire value is one uninterrupted trajectory guaranteed to
  reach a genuine exhaustive conclusion with no restart to recover from a
  bad branch -- wiring it into the shared signal regressed
  `grid_115.txt` (see the `unlimited_budget` entry above: newly solved,
  ~6-7s, by an *ordinary* restart-based worker) to a consistent 20s
  timeout, reproduced across repeated runs, then confirmed to still be
  hard (13-17+ minutes without solving) even given much longer than that.
  Excluded that one worker from the shared wiring entirely (neither reads
  nor writes it) -- restart-based workers still share among themselves,
  the one worker whose whole point is an undisturbed trajectory keeps
  its own.

  Measured effect on the 15x15 corpus, single-threaded unaffected (byte-
  identical to the pre-existing baseline, confirming the `num_threads>1`
  gate): default parallel mode showed a large, broad win --
  `grid_053.txt` 12.463s -> 0.655s, `grid_058.txt` 1.187s -> 0.120s,
  `grid_045.txt` newly solved within the 20s cap (previously always
  timed out) -- but did *not* recover `grid_115.txt`, which stayed a
  consistent timeout across repeated runs even after excluding the
  `unlimited_budget` worker, since it turns out `grid_115.txt` was
  originally solved by an *ordinary* restart-based worker, not the
  dedicated one -- the shared signal itself, not just the excluded
  worker's absence from it, redirects those workers' search order away
  from whatever used to get lucky there. One grid trading places for a
  broad win across the rest of the sample is this project's familiar
  pattern (see `SolveParallel`'s own "Measured effect" above for the same
  shape of result). Verified under ThreadSanitizer, including a real
  8-thread solve, not just the unit suite -- no data races reported.

  Did not clearly fix Hawksley's own variance, per direct testing: a
  14-threaded run with this change still hadn't solved after 12+ minutes
  in the one sample tried before the machine was needed for other
  verification. Kept anyway for the broad corpus win, which stands on its
  own regardless of whether it helps this one adversarial grid --
  consistent with this project's practice of judging a change by its
  overall effect on the benchmark corpus rather than by any single grid,
  hard or otherwise.
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
large real sample even when a smaller sample suggests it is.

**Also tried and reverted:** replacing `Propagate`'s O(total slots)
min-domain scan with a sorted `vector<int>` of just the currently-queued
slots (kept in ascending order specifically to preserve the original
scan's tie-breaking exactly, verified byte-identical on both samples) —
but this was a wash-to-slight-regression, not a win: the O(S) scan being
replaced was mostly cheap `vector<bool>` reads (the *real* per-slot cost,
an O(chunks) popcount, was already skipped for non-queued slots via
`continue`, same as before), so removing it saved little while the new
sorted-insert/erase maintenance cost was pure added overhead. Profile
before assuming a scan is the bottleneck, not just the largest loop bound
in the code. (A later session revisited the same underlying idea with an
*unsorted* list instead — see the "Propagation" section above — and that
version *did* win; the sorted-maintenance cost, not the core idea, was
what sank this one.)

**Also tried and reverted:** the Luby restart sequence (Luby, Sinclair &
Zuckerman, 1993 — see `docs/bibliography.md`) in place of the geometric
backtrack-budget growth (`kRetryGrowthFactor = 1.1`). Implemented as a
straight swap of the per-attempt budget formula, verified sound (all 15
tests pass) and benchmarked the same way as any other restart-affecting
change: three independent 30-grid real samples. Solve count was a wash
across all three (`grid_047.txt` and `grid_307.txt` gained,
`grid_328.txt` and `grid_424.txt` lost — two for two) but total time on
the 65 grids solved either way *regressed* by about 33% (14.3s → 19.0s),
almost entirely from two grids (`grid_128.txt`, `grid_290.txt`) taking
several seconds longer under Luby's oscillating budget before a large
enough attempt came up. Reverted: unlike nogood-recording-from-restarts
or the lazily-decayed crossing weights (both real, if mixed, net wins),
this traded away more time than the solve-count wash was worth.

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

## Correctness fixes

Two real bugs, both dormant at the recommended `min_score=40` and found
via a UBSan/ASan sweep plus a CLI fuzz check rather than in normal use:

- **Non-alphabetic dictionary entries caused undefined behavior.** The
  real wordlist has a handful of entries mixing letters and digits (e.g.
  `ENTREE3000`, `ARTHURC4CLARKE`) that survive loading at a low enough
  `min_score`. `Propagate`'s direct-lookup path reads a candidate word's
  raw character and shifts by `ch - 'A'` with no check that it's
  actually a letter -- for a digit this shifts by a negative amount,
  confirmed via UBSan (`shift exponent -N is negative`). Fixing just
  that shift exposed a second, related bug: a domain whose candidates
  are *all* non-letters at some position leaves `possible == 0`, which
  `(possible & (possible - 1)) == 0` can't distinguish from "exactly one
  bit set," so the single-bit fast path ran `__builtin_ctz(0)` (also
  undefined) and crashed further downstream. Fixed at the actual
  boundary instead of patching every reader: `Dictionary::LoadFromFile`
  now rejects any entry that isn't pure A-Z after uppercasing, the same
  as an empty word already is -- restoring the invariant every consumer
  of loaded words already assumed. Regression test in
  `tests/test_dictionary.cpp`; verified clean under UBSan and ASan
  afterward.
- **`xfill_cli`'s `min_score` argument crashed ungracefully on bad
  input.** `std::stoi(argv[3])` was parsed outside the `try`/`catch`
  block, so a non-numeric third argument (e.g. `xfill_cli grid.txt
  dict.txt abc`) terminated via an uncaught `std::invalid_argument`
  (SIGABRT) instead of the program's normal `error: ...` handling.
  Fixed by moving the parse inside the `try` block.
- **The restart backtrack-budget growth could overflow float->uint64_t,
  undefined behavior.** `Solve()`'s geometric backtrack-limit growth
  (`kRetryGrowthFactor`, see the "Restarts" section above) computes
  `static_cast<uint64_t>(static_cast<float>(limit) * 1.1f)` on every
  restart; once `limit` grows past roughly `2^64 / 1.1`, the float value
  exceeds `uint64_t`'s representable range and the cast is UB --
  confirmed via UBSan (`2.02914e+19 is outside the range of representable
  values of type 'unsigned long long'`), surfaced by the unit test suite
  itself: a trivially small search space can restart often and cheaply
  enough for 1.1x geometric growth to reach that range within a single
  test run. Fixed by clamping the grown value to `UINT64_MAX` before the
  cast instead of letting it overflow -- no real search benefits from a
  budget anywhere near that large anyway. Verified clean under UBSan
  afterward.

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
`benchmarks/grids/curated/sample_13x13.txt`) remain unsolved even after 15+
minutes at `min_score=40` -- and, tested after implementing
`SolveParallel` (see the "Restarts" section above), still unsolved after
20+ minutes with a 14-way portfolio search running, confirming this
really is the "genuinely hard regardless of search order" case the
theory below predicts, not just an unlucky single-threaded run. This is
an expected result, not a bug: per Anbulagan & Botea's phase-transition
study of crossword CSPs, some "hard region" instances stay expensive
under *any* search order, because the underlying instance is hard, not
because of a fixable search choice. Restarts fix a search that got
unlucky -- and running more of them at once, as `SolveParallel` does,
is still fundamentally the same fix -- but neither can turn a genuinely
hard instance easy. Most 20-second-cap timeouts on the real benchmark
set are not in this category — they solve within a couple of minutes
given a longer budget, and `SolveParallel` demonstrably rescues several
of them within the original cap — see the README's "Known limits"
section for the distinction on specific grids.

## Future work

Reasonable next steps, roughly in order of expected payoff for their
implementation cost:

- **Nogood learning via constraint-graph clustering** (Anbulagan &
  Botea's COMBUS). The *restart-scoped* form of nogood learning
  (Lecoutre, Sais, Tabary & Vidal — see the "Restarts" section above and
  the bibliography) is implemented and kept, with a real if modest net
  benefit. COMBUS's finer-grained idea -- scoping nogoods to independent
  constraint-graph regions -- is a different, still-untried refinement on
  top of that: it could plausibly make recorded nogoods more general
  (and so more likely to fire again in a later restart) by dropping
  irrelevant context from unrelated regions, rather than using this
  project's current simpler approach of recording the *entire* ancestor
  assignment regardless of relevance.
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

Since `SolveParallel` became `xfill_cli`'s default (see above), a plain
`bench_subset.py` run compares whichever worker's random attempt happened
to finish first -- real-time-scheduling-dependent, so it can report
different node counts for the *same grid* across two runs of identical
code. That's expected noise for judging `SolveParallel` itself, but it
pollutes the signal when isolating a change to the underlying single-
search algorithm (a solved grid's time swinging on scheduling luck rather
than the change under test). Use `bench_subset.py --threads 1` for that
case -- it reproduces worker 0's exact deterministic sequence, so
identical code always gives identical node counts and any time delta is
attributable to the change, not to scheduling.

Wall-clock comparisons also need the benchmark machine actually free --
easy to get wrong with long-running background experiments (this
project runs several: the `unlimited_budget` UNSAT-proving passes, the
extended-timeout external-solver comparison, etc.). A stray, forgotten
process from an earlier experiment inflated several timings during the
"word-choice randomization on restarts" investigation above by several
minutes before being noticed (`ps aux` showing a leftover single-
threaded `xfill_cli` run and a `test_unlimited2` process still
consuming full cores tens of minutes after they should have been
stopped) -- node/backtrack *counts* are unaffected by contention (same
deterministic sequence either way, per the paragraph above), but any
*wall-clock* number, especially one being compared against an external
tool's own single-run timing, should be re-measured after confirming
`ps aux` shows nothing else from this project still running.
