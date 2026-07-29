# Annotated Bibliography

Sources consulted while improving `Solver`, in the order they were read.
For each: what it is, then how it differs from this project's approach
and, where applicable, how it was actually used here.

*Session 2 addendum:* the entries below marked "(reread)" or "(new)" were
revisited/added in a follow-up pass specifically to find the next
concrete algorithm improvement. `rf-/ingrid_core`'s actual
`backtracking_search.rs` was reread in full (not just recalled from the
first pass) to get its restart mechanism's exact constants right rather
than approximating them from memory.

*Session 3 addendum:* one more source, below, was read at the user's
request and evaluated for whether it should change the algorithm. It did
not end up changing shipped behavior -- see its entry for why -- but the
evaluation itself (implement, benchmark, find a real bug, revert) is
recorded here in keeping with this project's benchmarking philosophy:
every claimed improvement needs a real before/after number, including
negative ones.

## Academic papers

### Beacham, Chen, Sillito, van Beek — "Constraint Programming Lessons Learned from Crossword Puzzles" (Canadian AI 2001)

**What it is.** An empirical study of 7 CSP encodings of crossword
filling × 8 backtracking algorithms × 3 variable-ordering heuristics
(34 valid combinations), run on 100 instances (50 grids × 2
dictionaries). The encodings range from a plain letter-per-cell model
(`m1`) through a dual word-slot model (`m2`, all binary constraints) to
a hybrid model with both cell and word variables (`m3`), plus SAT
encodings of each.

**How it differs / how it's used here.** Its central finding is that
model, algorithm, and heuristic choice are *mutually dependent* --
picking the best model in isolation, then the best algorithm for that
model in isolation, can land you orders of magnitude off the true best
combination. That's a direct validation of how this project actually
proceeded: rather than fixing an architecture up front, the solver's
propagation strategy, branching heuristic, and backtracking scheme were
each revised together across this conversation, re-benchmarked as a
unit each time. Concretely, the paper's `PAC` algorithms -- custom
arc-consistency propagators written per-model rather than a generic
solver -- are the closest analogue to this project's hand-written
`LetterMask`-bitset propagation, and PAC is what solves the most
instances in their study (88-92/100, versus 20/100 for plain forward
checking on the naive model). That's the paper's strongest signal in
favor of this project's approach of writing bespoke, bitset-based
propagation rather than reaching for a generic CSP library. One
technique from the paper *not* adopted: redundant prefix/suffix
projection constraints, which dramatically help forward-checking on
their weakest model (`m1`, 20 → 59/100 solved) but are largely subsumed
here already, since this project's per-slot word-domain propagation is
closer to their already-strong `m2`/`m3` models than to the weak
letter-only baseline that specifically needed the extra help.

### Anbulagan & Botea — "Crossword Puzzles as a Constraint Problem" (CP 2008)

**What it is.** Introduces Combus, a solver using a *hybrid* encoding
with both word-slot and letter-cell variables simultaneously (search
branches only on slots, but nogood records are kept in terms of the
lower-level cells). Also reports a detailed phase-transition study:
crossword hardness peaks in a middle range of dictionary size and of
blocked-cell count, with "hard region" instances taking longer than 24
hours even for Combus on realistic 23x23 grids.

**How it differs / how it's used here.** Not directly ported --
implementing genuine nogood learning (recording "this partial
assignment is unsatisfiable, don't retry it" clauses, scoped to
independent regions of the grid via constraint-graph clustering) is a
bigger lift than anything implemented this session. It's used here
diagnostically: it explains, with real data from a dedicated
specialized solver, why this project's own `sample_13x13.txt` and
`sample_15x15.txt` remain intractable even after the propagation and
branching improvements made in this session. That's not a bug in this
codebase; it's this project's grids landing in the same "hard region"
the paper identifies. Nogood learning via clustering is the most
promising *not-yet-implemented* next step for those specific grids (see
`docs/design.md`).

### Arbiser — "Practical Crossword Generation with Checkpoint Search" (IADIS 2005)

**What it is.** Introduces *checkpoint search*: rather than always
backtracking one level up when a branch is exhausted, mark high
branching-factor states as checkpoints, and jump back to the nearest
one when both the branch is exhausted *and* overall fill progress has
stalled. Also documents real-world crossword-quality constraints beyond
pure fillability: max repeated word "families" (regex-defined, e.g.
WORK/WORKER), max shared prefixes, and limits on rare/high-difficulty
words and plurals.

**How it differs / how it's used here.** This project's solver still
backtracks to the immediate parent on failure -- checkpoint search's
long-range jump was not implemented, since it targets a different
failure mode (getting stuck retrying minor variations near one bad
branch) than the one this session's `dom/wdeg` change targets
(chronically conflict-prone crossings). It's noted as a reasonable
complement, not a replacement: `dom/wdeg` changes *which slot* gets
picked, checkpoint search changes *how far back* a failure jumps. The
quality constraints (word-family limits, prefix limits) are a genuinely
different concern this project hasn't addressed at all -- this
project's `EnforceUniqueWordsOnce`/`used_by_length` machinery only
forbids *exact* duplicate words, not near-duplicates or thematically
repetitive fill.

### Botea & Bulitko — "Scaling Up Search with Partial Initial States in Optimization Crosswords" (SoCS 2021)

**What it is.** A two-stage search for the Romanian Crosswords
Competition (an *optimization* variant of crossword filling: maximize
the total length of "thematic" words placed, not just find any valid
fill). Stage one runs a deliberately over-aggressive-pruning search
(target score set unrealistically high) that fails fast but leaves
behind a high-quality partial assignment; that partial state, trimmed
of its most recently-added (least-confirmed) words, seeds a second
search for a full solution. Reports 7-224x speedups and many more
solved instances versus starting from scratch every time.

**How it differs / how it's used here.** This project's solver targets
plain feasibility (does a valid fill exist), not score optimization, so
the specific pruning mechanism (compare partial score + admissible
heuristic against a target) doesn't transfer directly -- there's no
"target score" in this project's problem. What's transferable, and
noted as a documented future direction rather than implemented, is the
general shape: run a cheap, aggressively-pruned pass to get a promising
partial assignment, then warm-start a full search from it instead of
from scratch. That's conceptually the same principle already applied
manually in this session when hunting for a fillable ~68-word 15x15
grid (generate many candidate block layouts, keep the one that fills
cleanly, rather than forcing one specific hard layout) -- this paper is
where that principle is named and measured rigorously.

### Gomes, Selman & Kautz — "Boosting Combinatorial Search Through Randomization" (AAAI 1998) *(new)*

**What it is.** Confirmed via the actual paper text (fetched from
`cs.cornell.edu/selman/papers/pdf/98.aaai.boost.pdf`, not a summary), not
about crosswords at all -- it studies backtracking search on scheduling,
planning, and circuit-synthesis instances. Its central empirical finding:
runtime for a *deterministic* complete search algorithm, plotted across
many similar problem instances, is often heavy-tailed -- a non-negligible
chance, at any point, of hitting an instance that takes exponentially
longer than anything seen so far, dragging the mean runtime toward
infinity. Crucially, they show the same heavy tail appears when a *single*
instance is re-run many times with a randomized tie-breaking rule and
different seeds -- so the hardness isn't really a property of the
instance, it's a property of the (instance, deterministic-algorithm)
pairing. Their fix: add controlled randomization to variable/value
selection and restart from scratch (keeping a time or backtrack cutoff)
whenever a run is taking too long. Reported results include several
previously-unsolved instances becoming solvable and speedups such as
logistics.d (108 min to 95 sec) and 3bit-adder-32 (>24 hrs to 165 sec).

**How it differs / how it's used here.** This is the theoretical
justification for the restart mechanism added to `Solver::Solve` this
session (see `rf-/ingrid_core` below for the concrete mechanism actually
ported) -- it's *why* "abandon this attempt and reseed" is expected to
help on exactly the kind of grids this project has struggled with
(`sample_13x13/15x15/21x21.txt`), rather than just being a plausible-
sounding idea. One thing from the paper not replicated: its formal
"boosted" search provably eliminates heavy tails to the *right* of the
median given certain conditions on the randomization; this project's
restart loop is the practical mechanism (geometric cutoff growth,
learned weights preserved across restarts) without reproducing that
formal guarantee -- the same pragmatic gap `ingrid_core` itself accepts.

### Meehan & Gray — "Constructing Crossword Grids: Use of Heuristics vs Constraints" (Aberdeen, 1997) *(new)*

**What it is.** Confirmed via the actual paper (`gtoal.com/scrabble/meehan/cross.pdf`), not a summary. Compares word-by-word vs. letter-by-letter grid instantiation, and a hand-written Prolog/C backtracker vs. a constraint-logic-programming solver (CHIP), on the same three benchmark grids as Ginsberg et al. 1990. For picking which pattern to fill next, it compares an exact match-count (`most_constrained`), a cheap ratio-based estimate, and a precomputed-probability estimate (`est_constrained`); for picking which word, it compares taking the first few dictionary matches, a random sample, and sampling weighted by how much choice each preserves for the rest of the grid.

**How it differs / how it's used here -- corroboration.** Its results independently validate two choices already made in this project, from a completely different implementation (Prolog/CHIP, not Rust) and era (1997, not this project's other sources): (1) exact match-count `most_constrained` selection was overall the best and most stable of its three fill strategies -- on its hardest 13x13 grid, exact counting took 226 backtracks vs. 3167.6 for the cheap ratio estimate and 2961 for the probability estimate -- which is exactly why this project's `dom/wdeg` uses `WordBitset::Count()` (an exact popcount) rather than a cheaper approximation; (2) it notes that `most_constrained` gets arc-consistency detection "for free," since an already-empty domain has zero matches and is picked (and fails) immediately, without a separate consistency check -- the same reason this project's `SelectBranchSlot` deliberately does not skip a slot whose masked domain is empty (see its doc comment in `solver.hpp`).

**How it differs / how it's used here -- an idea tried and reverted.** Its "seeding" technique (Section 2.4): since a deterministic fill/pick strategy produces the identical fill every time on the same grid, it seeds the search with one random word placed in one of the longest (highest-degree) patterns before instantiation starts, on the reasoning that perturbing a highly-connected pattern first ripples through the rest of the grid more than perturbing an arbitrary one would. This looked like a natural complement to this session's other restart work, so it was implemented: on each restart (never on the deterministic first attempt, consistent with this project's existing rule), place one uniformly-random word into a slot with the most crossings, then run the normal `dom/wdeg` search from there.

It was reverted after benchmarking exposed a real soundness bug, not just a missing win. On `sample_13x13.txt`, the seeded version reported "No solution found" after a single restart in ~2 seconds (610 total backtracks) -- while the unseeded restart mechanism, given the same 90-second window, hadn't even finished its *first* 500-backtrack-budget attempt. The seeded run wasn't actually faster at solving the real problem; forcing a specific random word into a high-degree slot shrinks the *reachable* search space so much that the (heavily constrained) remainder exhausts quickly -- but exhausting that constrained remainder only proves *that seed word doesn't work*, not that the whole grid is unsatisfiable. `Solve()`'s restart loop treats any attempt that exhausts without hitting its backtrack budget as definitive UNSAT, which is correct for the existing (unseeded) restarts, since every one of them searches the same full, unconstrained space, just in a different order -- but it's wrong once an attempt is seeded with an artificial constraint. A correct version would need to track seeded vs. unseeded attempts separately and only ever trust an *unseeded* exhaustion as authoritative (e.g. by alternating them), which is real added complexity for a technique whose actual benefit -- as opposed to this false-positive speed -- remains unmeasured. Given the choice between that complexity and a technique with no demonstrated real upside, it was backed out rather than fixed forward. `Solver::Solve` in `src/solver.cpp` still has one small permanent souvenir of this investigation: an `XFILL_DEBUG_RESTARTS`-gated stderr trace of each restart's cumulative backtracks and next budget, which is what made the bug visible in the first place and is generically useful for debugging future restart behavior.

## Practitioner writeups

### rainjacket/orca-solver — "How Orca Works" + Rust source (`crates/solver/src/`)

**What it is.** A Rust crossword fill engine. Read via its own writeup
plus the actual `propagate.rs`/`state.rs` source (fetched directly, not
taken from a summary) after an earlier session mis-identified the
project from title alone.

**How it differs / how it's used here.** This is where the two biggest
changes made across this project's recent sessions came from: (1)
trail-based incremental backtracking (`SolverState::save_domain`/
`pop_level`, ported as `Solver::SaveDomainOnce`/`Undo`) in place of
copying every slot's domain at every search node, and (2) queue-based
AC-3 propagation (`propagate_from_slots`, ported as `Solver::Propagate`)
with a subset-check before applying an intersection and a fast path
when every letter is still viable, in place of a fixed rescan of every
crossing until nothing changes. Not ported: Orca's headline
architectural difference of branching on individual grid *cells*
("which letter goes here") rather than whole word slots, its
per-arc-within-one-propagation-call letter cache, and its SIMD/
parallel/distributed search tiers -- all flagged as further upside
still on the table, not attempted due to the size of the rewrite
relative to this session's scope.

### rf-/ingrid_core (GitHub, Rust) *(reread)*

**What it is.** A production crossword-fill library (used by real
puzzle-construction tooling); explicitly credits Thanasis Balafoutis's
"Adaptive Strategies for Solving Constraint Satisfaction Problems" as
the basis for its search. Read from actual source
(`backtracking_search.rs`, `arc_consistency.rs`, `dupe_index.rs`) in the
first pass, and `backtracking_search.rs` specifically reread in full this
session to pin down its restart mechanism precisely rather than
approximate it.

**How it differs / how it's used here.** This is the direct source of
the `dom/wdeg` branching heuristic added in the first pass: every crossing
starts at weight 1, a wipeout bumps the responsible crossing's weight by
1, and all weights decay 1% toward 1 on every wipeout (`WEIGHT_AGE_FACTOR
= 0.99`, ported verbatim as `kWeightAgeFactor`) so the heuristic tracks
which crossings are *currently* troublesome rather than accumulating
grudges forever. Measured effect: `sample_11x11.txt` went from 339
nodes/88 backtracks to 268 nodes/26 backtracks (42ms → 19.6ms);
`sample_7x7.txt` now solves with zero backtracks.

This session, its restart mechanism (`find_fill`/`find_fill_for_seed`)
was reread and partially ported: `RETRY_GROWTH_FACTOR = 1.1` (grows the
per-attempt backtrack budget), a starting budget of 500 backtracks, and
`RANDOM_SLOT_WEIGHTS = [4, 2, 1]` (weighted-random choice among the best
few dom/wdeg-ranked slots instead of always the single best) are ported
close to verbatim as `kRetryGrowthFactor`/`kInitialBacktrackLimit`/
`kRandomSlotWeights`. Also ported: sharing `crossing_weights` across
restarts rather than resetting them, so dom/wdeg's learned "which
crossings are troublesome" information survives a restart even though the
search tree itself starts over. Two things deliberately *not* ported,
both explained in the `Solver` class comment in `solver.hpp`: (1)
`RANDOM_WORD_WEIGHTS` (randomizing *word* choice, not just slot choice) --
this project keeps word choice strictly `ScoreOrder`, since randomizing
it would conflict with the explicit score-quality-first goal validated by
the "prefers the higher-scored word" test; (2) `ADAPTIVE_BRANCHING_THRESHOLD`
/"stickiness" (stay on the previous slot if a new one isn't much better,
to avoid thrashing) -- this is meaningful in ingrid_core's iterative loop,
where a slot can stay the active target across several word attempts, but
doesn't map onto this solver's recursive design, where `Assign()`
immediately collapses a chosen slot to a singleton and removes it from
consideration entirely -- there's no "still open" slot left to stick to.
One more deviation, found via benchmarking rather than by reading:
`ingrid_core` randomizes slot choice on *every* attempt, including the
first; doing that here regressed grids the plain greedy choice already
solved well (e.g. `sample_7x7.txt`: 22 nodes/0 backtracks greedy vs. 2597
nodes/93 backtracks always-randomized), so this project only randomizes
slot choice on restarts (attempt > 0), keeping attempt 0 fully
deterministic. Also notable but unused: its `DupeIndex` generalizes
this project's exact-word-only duplicate check into an n-gram-windowed
"forbid words sharing a long substring" check (the real-world "max
shared substring" quality constraint also seen in Arbiser's paper and
in `crosswords-rs`'s constraints, below) -- a reasonable next step for
fill *quality*, distinct from the performance work done this session.

### afck/crosswords-rs (GitHub, Rust)

**What it is.** A crossword generator; read from actual source
(`word_constraint.rs`, `main.rs`), not just the README.

**How it differs / how it's used here.** Its `main.rs` generates
several candidate grids via `--samples N` and keeps the highest-scoring
one (score rewards word count and preferred/"favorite" words, penalizes
empty border cells and non-favorite fill) -- the same "generate many,
keep the one that fills cleanly" principle already applied by hand in
this session to find a fillable ~72-word all-over-interlock 15x15 (see
`benchmarks/grids/sample_15x15_interlock.txt`), corroborating it as a
recognized technique rather than an improvised workaround. Its CLI also
exposes `min_crossing`/`min_crossing_percent` -- a *quality* constraint
("every word must cross at least N others" / "at least X% of a word's
letters must belong to a perpendicular word") that this project has no
equivalent of: this project's solver only asks "is this fill valid,"
never "is this fill well-interlocked." Not adopted, but a clear gap
between what this project checks for and what a real puzzle-quality
tool checks for.

### snowan.gitbook.io — "Design Crossword Puzzle Solver"

**What it is.** A blog-style writeup of a crossword solver design.

**How it differs / how it's used here.** Its core proposal --
cell/slot CSP variables, MRV variable ordering, AC-3 propagation after
each assignment, dictionary indexed by length and by pattern (a trie)
-- is architecturally the same shape this project already had before
this session, so it mainly serves as independent corroboration that
this project's baseline design isn't idiosyncratic. Its one idea not
present in this project at all: decomposing the backtracking search
tree into a work queue and distributing subtrees (past some depth
threshold) across stateless workers. Not implemented -- this project's
solver is single-threaded throughout -- but it's the same shape as
Orca's own parallel/distributed search tier, so it's flagged in
`docs/design.md` alongside that rather than as a separate idea.

### cs.columbia.edu/~evs/ais/finalprojs/steinthal — course project page

**What it is.** A historical/pedagogical writeup of a naive
letter-by-letter crossword solver: no bitsets, a grid of raw
possible-character lists per cell, backward-tracing "what word caused
this letter" for undo.

**How it differs / how it's used here.** Used as a negative example,
not a source of technique: the author reports loading a full dictionary
took over 20 minutes on period hardware, and the project never got
past a "pen and paper" (i.e., non-working) implementation, explicitly
attributing this to the approach's complexity. It's a useful data point
for *why* this project represents domains as `WordBitset`s with
precomputed per-position letter masks rather than per-cell candidate
lists scanned against a raw dictionary -- the naive approach this page
describes is close to what a first attempt at this problem looks like
before bitset indexing, and it doesn't scale.

## Consulted but not about grid-filling algorithms

### Zeinalipour, Iaquinta, et al. — "Building Bridges of Knowledge: Innovating Education with Automated Crossword Generation" (ICMLA 2023)

**What it is.** Introduces a Turkish educational crossword generator
using LLMs (GPT-2/GPT-3, BERT) to generate and verify *clues* for given
answers, for language-learning use. Part of a series (Italian, Arabic,
French follow-ups).

**Why it's not used.** Entirely orthogonal to this project: it assumes
the grid and answers already exist and focuses on natural-language clue
generation/verification, not on filling a grid with words at all.

### Arsov, Kitanovski, Jovanov — "Crossword Generation as a Constraint Satisfaction Problem Using Parallel Processing and Lemmatization" (ICT Innovations 2024 / Springer, 2025)

**What it is, as far as could be confirmed.** A CSP-based crossword
generator incorporating parallel processing and lemmatization (a
linguistic normalization step, likely needed for a morphologically rich
target language's dictionary).

**Why it's not used.** The full text sits behind a Springer paywall
that this session's tooling couldn't get past (only the title, authors,
and venue could be confirmed, via search rather than the paper itself).
Given that, no technique from it was ported -- the parallel-processing
angle is directionally the same idea already flagged from the Orca
writeup and the snowan blog (distribute independent subtrees across
workers), so it isn't a missed distinct idea, just an unread source.
