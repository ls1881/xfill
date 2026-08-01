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
  entire thread. Still complete: `SolveParallel` only reports
  unsatisfiable once every worker has independently, genuinely exhausted
  its own search (none merely cancelled), which reduces directly to
  `Solve()`'s own already-established completeness, run N times.

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
