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

*Session 4 addendum:* the user asked whether there's a crossword
equivalent of the "critical junction" structure road-routing algorithms
(contraction hierarchies, transit-node routing) exploit -- a small set of
nodes that most long routes must pass through, knowing which prunes the
search space enormously. The academic entry below (Dechter) is the real
answer to that question; see it for what was implemented, measured, and
concluded.

*Session 5 addendum:* the user asked for an open-ended pass -- review the
algorithm, research other CSP approaches, and iterate: implement, measure
against the real benchmark set, keep what helps, revert and document what
doesn't. Three things came out of this pass, in the order investigated:
(1) the "Learning" half of the Dechter (1990) paper below already had
backjumping investigated and reverted in session 4, but not learning
itself -- this session finished and honestly benchmarked it (see that
entry's new subsection); it regressed the same way backjumping did, for
what looks like the same underlying reason. (2) A restart-strategy
literature check (Luby et al. 1993 vs. Walsh's geometric strategy) found
this project's existing choice (geometric, ported from `ingrid_core`) is
already the one the wider SAT/CSP literature converged on in practice, so
no code change followed -- see the new corroboration entry below. (3) A
fresh `sample`-profiling pass on a currently-timing-out real grid found a
genuine, if modest, remaining constant-factor win unrelated to any of the
above: see `docs/design.md`'s roadmap for the profiling numbers and what
changed (`WordBitset`'s methods moved into the header so they can actually
be inlined at their call sites, since this project builds without LTO).

*Session 6 addendum:* the user pushed back on session 5's conclusion --
"should be able to fill all the scraped_15x15" -- rather than accepting
the roughly-30%-solved status quo as a fixed ceiling. This wasn't a new
technique to research so much as a question this project had never
actually asked: every session up through session 5 benchmarked at
`min_score=50` without ever testing whether *that specific number* was
appropriate, and it turned out to be the single biggest lever pulled in
this project's history, well ahead of any search-algorithm change --
`min_score=40` alone roughly triples the practical solve rate (30% → 78%
on a 100-grid real sample at a 20s cap), and most of the *remaining*
20s-cap timeouts turn out to just need a couple more minutes of patience,
not a fundamentally harder search or an even bigger dictionary. Full
investigation, numbers, and the honest remaining gap (a real if smaller
hard core, e.g. `sample_13x13.txt`, unresolved after 15+ minutes even
with the bigger dictionary) are in `docs/design.md`'s roadmap (step 8)
rather than repeated here, since this was an empirical
dictionary-configuration investigation rather than a technique drawn from
an external source -- there's no paper or codebase to annotate, just this
project's own benchmark set finally being asked the right question.

### Dechter -- "Tractable Structures for Constraint Satisfaction Problems" (book chapter, 2006) *(new)*

**What it is.** A survey of graph-structure-based tractability results for
CSPs, read from the actual chapter text (not a summary) -- and, strikingly,
its very first worked example is a crossword puzzle used to introduce
primal/dual constraint graphs. Covers two families of techniques: width-
based (tree-width/induced-width, join-tree clustering, cluster-tree
elimination) and cutset-based (cycle-cutset, w-cutset, and -- most
relevant here -- decomposition into *non-separable components*: a
connected graph has a *separation node* if some node's removal
disconnects it, and a subgraph with no such node is *non-separable* /
*biconnected*; a linear-time DFS (Hopcroft & Tarjan's classic algorithm)
finds all of them at once, and they're always interconnected in a tree,
which is itself a valid tree-decomposition).

**How it differs / how it's used here.** Full tree-decomposition (join-
tree clustering, cluster-tree elimination) was not ported -- those
algorithms are built around relational join/projection over tuples,
which doesn't map onto this project's bitset-domain backtracker without
a much larger rewrite than is justified here, and Dechter's own
complexity results show they trade time for space *exponential in
tree-width*, which isn't obviously a win over the existing dom/wdeg
search on grids that aren't already known to have small tree-width. What
*was* implemented is the lighter non-separable-components idea: `Solver`
now computes connected components of the slot-crossing graph once, via
one BFS pass in the constructor (`slots_by_component_`), and
`SelectBranchSlot` only ever offers candidates from the lowest-indexed
component that still has unassigned slots -- fully settling (or proving
impossible) one component before starting the next, which is always at
least as good as interleaving since components sharing no crossing can
never help each other's search, and never *worse* since it's the same
backtracking tree with a restricted candidate set, not a separate search
(this matters because the grid-wide no-duplicate-words rule *does* still
couple different components, so they can't just be solved as fully
independent, parallel sub-problems without coordination).

**Measured effect.** Zero difference on any single-component grid
(identical node/backtrack counts on every grid in `benchmarks/grids/` and
the real scraped set -- exactly the expected no-op, since with one
component "the lowest-indexed component with unassigned slots" is just
"the whole grid," same as before). A constructed stress test (4 tiled
copies of `sample_9x9.txt` as independent components in one grid) showed
the real effect: 323,978 nodes/9.33s before vs. 125,521 nodes/3.59s after
-- about 2.6x fewer nodes and faster.

**The honest, complete answer to "is there a crossword equivalent."**
Yes, structurally -- non-separable components (and their finer-grained
cousin, articulation points *within* one component) are the real
crossword analogue of the "critical junction" nodes in road-network
routing. But checking whether that structure actually *exists* in real
grids (all 500 in `benchmarks/grids/scraped_15x15/`, via a temporary
Tarjan's-algorithm diagnostic) found it essentially doesn't: all 500 have
exactly one connected component, and 485/500 (97%) have *zero*
articulation points even within that one component; the remaining 15
have only 1-4. This makes sense in hindsight -- crossword constructors
deliberately avoid weak, separable interlock as a matter of puzzle
quality, so well-constructed grids are close to maximally
non-separable by design. The technique is real, sound, and kept (it's a
free no-op for the common case and a genuine ~2.6x win for any grid that
*does* have independent regions, like `sample_9x9.txt`'s degenerate
disconnected sibling in `benchmarks/grids/synthetic/`), but it isn't the
lever that will move the needle on why real 15x15s like
`grid_013.txt`/`grid_017.txt`/etc. still time out -- those grids are hard
because they're densely, well interlocked, which is the opposite
condition from what this technique exploits. That's consistent with
Anbulagan & Botea's phase-transition point (see their entry above): the
remaining hardness is a property of the instance under *any* search
order, which is squarely nogood learning's territory, not a structural
shortcut like this one.

### Dechter -- "Enhancement Schemes for Constraint Processing: Backjumping, Learning, and Cutset Decomposition" (Artificial Intelligence, 1990) *(new)*

**What it is.** The original paper behind graph-based backjumping (GBJ),
read from the actual text (not a summary) specifically to get the
algorithm exactly right, since an unsound implementation here risks
something worse than slowness: falsely reporting no solution when one
exists. GBJ's idea: when standard backtracking hits a dead end at
variable X, retrying the *immediately preceding* decision is often
pointless if that decision had nothing to do with why X failed. GBJ
instead computes X's "parents" -- the already-assigned variables X is
actually constrained by, read straight off the constraint graph -- and
jumps back directly to the most recent one, skipping any decisions in
between that couldn't possibly matter. The paper proves this is sound: a
solution is never missed, because the parent set for a whole cascade of
dead-ends is accumulated (not just recomputed fresh at each one), so a
later, deeper conflict correctly folds in everything an earlier one
already ruled out.

**How it differs / how it's used here.** Implemented directly in
`Backtrack`: an exhausted slot's currently-assigned crossing neighbors
become its "parents," unioned into an accumulator, and the most recently
assigned one becomes a jump target that every ancestor stack frame checks
before trying its next candidate. One deliberate deviation from the
paper: Dechter's proof assumes a *fixed* variable order, so a parent
stays valid for the rest of the search; this project's dom/wdeg order is
dynamic (which slot gets picked next depends on current domain state,
not a preset sequence), so an accumulated parent from a much earlier,
already-resolved cascade could reference a slot since reassigned to
something unrelated. To keep the implementation unconditionally sound
under dynamic ordering, the accumulator was reset every time search made
fresh forward progress, restricting jumps to *within* one uninterrupted
cascade of dead-ends -- where the currently-assigned slots exactly match
the call stack, exactly matching the paper's model. This is strictly
more conservative than the full algorithm (forgoes some jumps *across*
cascades) but never trades away soundness for it.

**Measured effect -- implemented soundly, reverted anyway.** All 15
tests passed, including the "no solution exists" ones, so the soundness
goal was met. But the net performance effect on the 20-grid real sample
was a clear regression: solved count dropped from 6 to 4, with two
grids that solved comfortably before (`grid_016.txt`, `grid_126.txt`)
now timing out at 20s. Individual results were genuinely mixed --
`sample_11x11.txt` improved 4x (268 → 66 nodes) -- but the aggregate
was worse, so it was reverted rather than kept for a cherry-picked
win. Best-guess explanation: GBJ was designed as a standalone
enhancement to plain chronological backtracking, not to compose with a
search that already has two other adaptive mechanisms doing related
jobs -- dom/wdeg's crossing-weight learning already biases future slot
*selection* toward chronically troublesome crossings, and randomized
restarts already provide an escape hatch from a demonstrably stuck
attempt. Forcing the *next slot to retry* to be "the most recent
assigned neighbor of the failure," as GBJ does, can override dom/wdeg's
own (separately tuned, already-benchmarked) judgment about what's
actually most urgent to try next, and this project's restarts already
handle the "the search is stuck" case GBJ is also aimed at, from a
different angle. Untangling that interaction well enough to make GBJ a
net positive here would need its own dedicated investigation rather
than a bolt-on; not pursued further this round.

**Learning -- also implemented, also reverted (session 5).** This same
paper's title has two halves, "Backjumping" and "Learning," and only the
first was evaluated in session 4. This session implemented the second:
plain nogood learning (not the clustered/COMBUS variant below -- see
Anbulagan & Botea), scoped deliberately narrowly to keep it unconditionally
sound without approximating anything. Every slot's domain carries a
`responsible_` bitset of which currently-assigned slots actually narrowed
it (credit flows transitively through unassigned, propagation-only
intermediaries too, riding the same trail as the domain itself so
backtracking undoes both together); whenever a domain empties with zero
candidates having been tried -- either mid-propagation, or in `Backtrack`
when the global used-word mask alone wipes out an already-nonempty domain
-- that responsible set (plus, for the used-word case, whichever assigned
slots own the specific masking words) is recorded as a nogood, capped in
size and count (Dechter's "i-bounded learning") and deduped by hash.
`Assign` then checks, before paying for a full propagation cascade,
whether the assignment just made completes any previously-recorded
nogood mentioning that slot -- if so it fails immediately. Unlike the
reverted backjumping half, this never changes *where* the search
backtracks to, only adds an early-exit check on top of the otherwise
unchanged chronological search, so there was real reason to expect it
might avoid backjumping's regression.

It didn't. Benchmarked on the same 20-grid real sample (seed 42,
`min_score=50`, 20s cap): solved count dropped from 6 to 5, with
`grid_126.txt` going from solved (7.2s) to timeout, and `grid_016.txt`
getting markedly slower (16,636 → 78,040 nodes; 2,583 → 12,696 backtracks;
4 → 13 restarts; 0.88s → 5.06s) despite the mechanism visibly working --
`grid_016.txt` alone recorded 7,488 nogoods and used them to short-circuit
941 assignments without running propagation. The pruning is real; the
net effect is still worse. Best-guess explanation, and it's the same one
as backjumping's: both `grid_016.txt` and `grid_126.txt`'s regressions
came with a jump in `restarts` (4→13, and enough to newly time out,
respectively), meaning nogood-driven pruning changed *how much work each
restart attempt does before hitting its backtrack budget and giving up*
-- not the total work, which any sound pruning mechanism should only ever
reduce, but its *distribution* across attempts. Since each restart reseeds
`SelectBranchSlot`'s tie-breaking RNG independently (see the restart
mechanism in `solver.hpp`), an attempt that fails faster/differently
because of nogood pruning explores a differently-shaped subtree before
giving up, which changes which attempt number turns out to be the lucky
one -- for these two grids, unluckily. Reverted (`git checkout` back to
this session's starting commit for `include/xfill/solver.hpp` and
`src/solver.cpp`) rather than kept for a cherry-picked win, same standard
as backjumping. The emerging pattern across both attempts: any mechanism
that prunes based on the *current* search state interacts with this
project's restart+dom/wdeg combination in ways that are very easy to get
soundly right and very hard to get net-positive, because the real lever
being pulled is each restart's random-seed trajectory, not the raw amount
of search work. A future attempt at either idea should probably measure
*that* interaction directly (e.g. restart-count and per-attempt backtrack
distributions, not just aggregate nodes) rather than treating "it's sound
and it prunes" as sufficient justification to expect a net win.

### Universal vs. geometric restart strategies -- corroboration, no code change (session 5)

**What it is.** Two restart-schedule results checked against the
existing choice in `Solve()` (`kRetryGrowthFactor = 1.1`, ported from
`ingrid_core`, itself citing Balafoutis): Luby, Sinclair & Zuckerman's
"Optimal Speedup of Las Vegas Algorithms" (1993), which proves a specific
restart sequence is *log-optimal* when the underlying runtime
distribution is unknown; and Walsh's geometric-growth strategy, adopted
by MiniSat from version 1.13 onward and widely credited since as the
practically better-performing choice despite lacking Luby's worst-case
guarantee.

**How it differs / how it's used here.** Not implemented, because there
was nothing to fix: this project already uses geometric growth, which is
the side the practical literature (not just MiniSat's own choice, but the
broader SAT/CSP-solver-engineering consensus that followed it) has
converged on, precisely because Luby's optimality guarantee is a
worst-case bound that doesn't reflect typical behavior. This is
corroboration in the same spirit as the Meehan & Gray entry below: an
independent source, read specifically to find a possible improvement,
that instead validated a choice already made -- worth recording so a
future session doesn't re-propose switching to Luby without knowing this
was already checked.

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
