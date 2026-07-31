# Annotated Bibliography

Sources that shaped the solver as it exists today. For each: what it is,
and what this project took from it (or explicitly didn't, and why).

## Sources behind the current implementation

### Beacham, Chen, Sillito, van Beek — "Constraint Programming Lessons Learned from Crossword Puzzles" (Canadian AI 2001)

An empirical study of 7 CSP encodings of crossword filling × 8
backtracking algorithms × 3 variable-ordering heuristics. Its central
finding is that model, algorithm, and heuristic choice are *mutually
dependent* — picking each in isolation can land you orders of magnitude
off the best combination, which is why this project's propagation,
branching, and backtracking scheme are tuned and benchmarked together
rather than independently. Its `PAC` algorithms (custom arc-consistency
propagators written per-model) are the closest analogue to this
project's hand-written `WordBitset` propagation, and outperformed
generic forward checking by 4-5x in their study — the strongest
external signal in favor of bespoke bitset propagation over a generic
CSP library.

### rainjacket/orca-solver — "How Orca Works" + Rust source (`crates/solver/src/`)

A Rust crossword fill engine. Source of two of this solver's core
mechanisms: (1) trail-based incremental backtracking
(`SolverState::save_domain`/`pop_level`, ported as
`Solver::SaveDomainOnce`/`Undo`) — only the domains a decision actually
touches get snapshotted, instead of copying every slot's domain at every
search node; (2) queue-based AC-3 propagation (`propagate_from_slots`,
ported as `Solver::Propagate`) with a subset-check before applying an
intersection and a fast path when every letter is still viable, instead
of a fixed rescan of every crossing until nothing changes. Not ported:
Orca's branching on individual grid cells rather than whole word slots,
its per-arc letter cache, and its SIMD/parallel/distributed search
tiers — all larger rewrites than this project has taken on.

### rf-/ingrid_core (GitHub, Rust)

A production crossword-fill library, crediting Thanasis Balafoutis's
"Adaptive Strategies for Solving Constraint Satisfaction Problems" as
the basis for its search. Source of both `dom/wdeg` branching and the
restart mechanism:

- **`dom/wdeg`.** Every crossing starts at weight 1; a wipeout bumps the
  responsible crossing's weight by 1; all weights decay 1% toward 1 on
  every wipeout (`WEIGHT_AGE_FACTOR = 0.99`, ported as
  `kWeightAgeFactor`), so the heuristic tracks which crossings are
  *currently* troublesome rather than accumulating grudges forever.
- **Restarts.** `RETRY_GROWTH_FACTOR = 1.1` (per-attempt backtrack budget
  growth), a starting budget of 500 backtracks, and
  `RANDOM_SLOT_WEIGHTS = [4, 2, 1]` (weighted-random choice among the
  best few dom/wdeg-ranked slots instead of always the single best) are
  ported close to verbatim as `kRetryGrowthFactor`/
  `kInitialBacktrackLimit`/`kRandomSlotWeights`. `crossing_weights` is
  shared across restarts rather than reset, so dom/wdeg's learned state
  survives a restart even though the search tree itself starts over.

Two things deliberately *not* ported, both explained in the `Solver`
class comment in `solver.hpp`: `RANDOM_WORD_WEIGHTS` (randomizing word
choice, not just slot choice) — this project keeps word choice strictly
score-ordered, since randomizing it would conflict with the
score-quality-first goal; and `ADAPTIVE_BRANCHING_THRESHOLD`
("stickiness", staying on the previous slot if a new one isn't much
better) — meaningful in `ingrid_core`'s iterative loop, but this
solver's recursive design collapses a chosen slot to a singleton
immediately, so there's no "still open" slot to stick to. One more
deviation found via benchmarking: `ingrid_core` randomizes slot choice
on *every* attempt, including the first; this project only randomizes on
restarts (attempt > 0), keeping attempt 0 fully deterministic, since
always-randomizing regressed grids the plain greedy choice already
solved well.

Also notable but unused: `ingrid_core`'s `DupeIndex` generalizes
exact-word-only duplicate checking into an n-gram-windowed "forbid words
sharing a long substring" check — a real-world fill-quality constraint
this project doesn't currently enforce.

### Gomes, Selman & Kautz — "Boosting Combinatorial Search Through Randomization" (AAAI 1998)

Studies backtracking search on scheduling, planning, and
circuit-synthesis instances (not crosswords). Central finding: runtime
for a deterministic complete search algorithm is often heavy-tailed — a
non-negligible chance of hitting an instance that takes exponentially
longer than anything seen so far — and the same heavy tail appears when
a single instance is re-run with a randomized tie-breaking rule and
different seeds, meaning the hardness is a property of the (instance,
algorithm) pairing, not the instance alone. Their fix — randomize
variable/value selection and restart from scratch past a time/backtrack
cutoff — is the theoretical justification for this solver's restart loop
(the concrete mechanism comes from `ingrid_core`, above): it's why
"abandon this attempt and reseed" helps on grids that would otherwise
blow up under one unlucky branch order.

### Lecoutre, Sais, Tabary & Vidal — "Nogood Recording from Restarts" (IJCAI 2007)

Studies nogood learning specifically combined with randomized restarts
(general CSP benchmarks and a real-world radio frequency assignment
problem, not crosswords). Its central move: record nogoods only from the
*last branch* before a restart, and only from decisions that were
genuinely, completely refuted (a value tried and its whole subtree
exhausted without success) — never from a branch merely cut short by the
restart's own cutoff. This keeps the *number* of nogoods bounded by the
number of restarts (polynomial, not exponential), and — critically for
this project's history — is a structurally different mechanism from the
plain nogood learning tried and reverted in an earlier session (which
recorded from *every* domain wipeout throughout search, changing how much
work the *same* attempt did before hitting its own budget, which is what
made that attempt regress). Recording only at restart boundaries, from
already-exhausted branches, doesn't have that failure mode: it can only
ever save a *later* restart from redoing a dead end an *earlier* one
already fully proved.

**How it's used here.** Adapted rather than ported directly, since the
paper's algorithm assumes binary (assign/refute one variable-value pair
at a time) branching with an explicit growing decision log, while this
solver branches d-way (try many candidate words for one slot in
sequence) and has no such log. The adaptation used here: when a slot's
candidate loop runs to completion (every candidate genuinely tried and
undone with the abort flag still clear at each step) and that specific
exhaustion is what pushes the backtrack count over the current attempt's
limit, the entire current assignment (every other currently-assigned
slot's word) is recorded as one nogood — sound because assigning more
context on top of an already-proven dead end can only keep it a dead
end, never un-prove it. `Solver::RecordNogoodFromDeadEnd` /
`NogoodForbiddenWords` in `solver.cpp`/`solver.hpp` implement this;
see `docs/design.md` for the measured effect.

### Dechter — "Tractable Structures for Constraint Satisfaction Problems" (book chapter, 2006)

A survey of graph-structure-based tractability results for CSPs. Source
of this solver's component-restricted branching: a connected graph
decomposes into *non-separable* (biconnected) components, found in one
linear-time BFS/DFS pass, and a search can fully settle one component
before starting the next, since components sharing no crossing can
never help or hurt each other's search. `Solver` computes these
components once at construction (`slots_by_component_`), and
`SelectBranchSlot` only ever offers candidates from the lowest-indexed
component with an unassigned slot. Dechter's heavier techniques (full
tree-decomposition via join-tree clustering) were not ported — they're
built around relational join/projection over tuples, which doesn't map
onto this project's bitset-domain backtracker without a much larger
rewrite, and their cost is exponential in tree-width, which isn't
obviously a win over dom/wdeg search on grids that aren't already known
to have small tree-width.

Measured effect: zero difference on any single-component grid (the
common case — real, well-built crosswords are almost always fully
interlocked, so this is a free no-op there), and roughly 2.6x fewer
nodes on a constructed grid with genuinely independent regions (see
`benchmarks/grids/synthetic/disconnected_15x15.txt`).

### Meehan & Gray — "Constructing Crossword Grids: Use of Heuristics vs Constraints" (Aberdeen, 1997)

Compares word-by-word vs. letter-by-letter grid instantiation and
several slot-selection heuristics on hand-built benchmark grids.
Corroborates two choices already made in this project, independently and
from a completely different implementation (Prolog/CHIP, 1997): (1)
exact match-count selection (`most_constrained`) was overall the best
and most stable of the fill strategies they tried, which is why this
solver's `dom/wdeg` uses `WordBitset::Count()` — an exact popcount —
rather than a cheaper approximation; (2) `most_constrained` gets
arc-consistency detection "for free," since an already-empty domain has
zero matches and is picked (and fails) immediately without a separate
consistency check — the same reason `SelectBranchSlot` deliberately does
not skip a slot whose masked domain is empty.

## Consulted for context, not adopted

- **Anbulagan & Botea — "Crossword Puzzles as a Constraint Problem" (CP
  2008).** Reports a phase-transition study: crossword hardness peaks in
  a middle range of dictionary size and blocked-cell count, with "hard
  region" instances taking longer than 24 hours even for their
  specialized solver (Combus) on realistic grids. Used diagnostically —
  it explains why a handful of this project's real benchmark grids
  remain intractable even after every implemented optimization, rather
  than pointing to a bug. Combus's clustered nogood learning is the most
  promising *not-yet-implemented* next step for those grids (see
  `docs/design.md`).
- **Dechter — "Enhancement Schemes for Constraint Processing: Backjumping, Learning, and Cutset Decomposition" (Artificial Intelligence, 1990).**
  Graph-based backjumping and plain nogood learning were both
  implemented soundly and benchmarked, and both were reverted: each
  regressed the real-grid solve rate despite genuine per-node pruning,
  because both interact with this project's restart+dom/wdeg combination
  by changing how much work each restart attempt does before giving up —
  which perturbs *which* restart's random seed ends up solving a grid,
  for the worse on net across the benchmark set. Not part of the current
  solver.
- **Arbiser — "Practical Crossword Generation with Checkpoint Search" (IADIS 2005).** Checkpoint search (long-range backtrack jumps to a
  marked high-branching-factor state when fill progress stalls) targets
  a different failure mode than dom/wdeg and restarts already cover, and
  wasn't implemented. Also documents real-world fill-quality constraints
  (repeated word "families", shared-prefix limits) that this project
  doesn't enforce beyond exact-duplicate rejection.
- **Botea & Bulitko — "Scaling Up Search with Partial Initial States in Optimization Crosswords" (SoCS 2021).** A two-stage warm-start search
  for a score-*optimization* crossword variant. This solver targets
  plain feasibility, so the specific pruning mechanism doesn't transfer,
  but the general shape — cheap aggressively-pruned pass to get a
  promising partial assignment, then a full search seeded from it — is a
  plausible future direction, not currently implemented.
- **afck/crosswords-rs, snowan.gitbook.io "Design Crossword Puzzle Solver".** Independent corroboration that this project's architecture
  (bitset/slot CSP, MRV-family ordering, AC-3-style propagation,
  generate-many-candidates-and-keep-the-best grid construction) isn't
  idiosyncratic. Their parallel/distributed search ideas aren't
  implemented — this solver is single-threaded throughout.
- **cs.columbia.edu/~evs/ais/finalprojs/steinthal.** A naive
  letter-by-letter solver with no bitset indexing, cited as the negative
  example `WordBitset`'s precomputed letter masks are meant to avoid
  (its author reports a 20+ minute dictionary load and never got past a
  non-working implementation).
- **A project-original idea: seeding dom/wdeg's initial crossing weights from letter-collision probability.** Implemented and benchmarked across
  several prior-strength values; consistently regressed total search
  cost on the real benchmark set relative to the uniform-1 starting
  weights every published dom/wdeg variant uses. Not part of the current
  solver.
- **Zeinalipour et al. (ICMLA 2023), Arsov et al. (ICT Innovations 2024).** LLM-based clue generation and a paywalled parallel-CSP
  generator, respectively — orthogonal to grid-filling and not used.
