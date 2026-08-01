#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "xfill/dictionary.hpp"
#include "xfill/grid.hpp"

namespace xfill {

struct Solution {
  std::unordered_map<int, std::string> assignment;  // slot id -> word
};

struct SolverStats {
  uint64_t nodes = 0;
  uint64_t backtracks = 0;
  // Number of times Solve() abandoned a search attempt after it hit its
  // backtrack budget and restarted from the root with a larger budget. 0
  // means the first attempt found the answer (or proved none exists)
  // outright.
  uint64_t restarts = 0;
};

// Result of Solver::SolveParallel: the solution (if any), plus stats.
// When a solution is found, `stats` is whichever thread's search
// actually produced it (matching Solve()'s usual meaning: the real path
// to the answer). When every thread instead independently proves the
// grid unsatisfiable, there's no single "the" search to report, so
// `stats` is the sum across all of them -- the total work needed to
// prove it. num_threads is how many were actually launched, which may
// be less than requested if hardware_concurrency() couldn't be read.
struct ParallelSolveResult {
  std::optional<Solution> solution;
  SolverStats stats;
  unsigned num_threads = 0;
};

// CSP solver: incremental backtracking with real (cascading) AC-3
// propagation at every node, kept cheap via a work-queue and a few
// short-circuits (design lifted from rainjacket/orca-solver's Rust fill
// engine):
//
//  - Propagation is queue-driven, not a fixed rescan of every crossing:
//    only slots whose domain actually shrank get re-examined, and the
//    queue always pops the currently-smallest domain first so wipeouts
//    surface as early as possible.
//  - A narrowing step is skipped entirely if the neighbor's domain is
//    already a subset of the incoming filter (nothing would change), and
//    also if every letter is still viable at that position (the filter
//    would be a no-op).
//  - Backtracking is trail-based: assigning a slot snapshots each domain
//    it touches (at most once per decision level -- a domain narrowed
//    twice in one cascade only needs its *original* value saved) so
//    undoing restores exactly what one decision changed, not a copy of
//    every slot's domain.
//
// The same Propagate() runs once at the root (seeded from every slot, to
// prune before search starts) and again after every single assignment
// during search (seeded from just that slot, cascading through however
// many neighbors it actually affects) -- there's no separate "cheap
// forward-check" tier anymore, because the above short-circuits make full
// propagation itself cheap enough to run everywhere.
//
// Branching uses dom/wdeg (domain size over weighted degree), following
// rf-/ingrid_core's port of Balafoutis's "Adaptive Strategies for Solving
// Constraint Satisfaction Problems": every crossing starts at weight 1,
// and whenever propagation wipes out a domain, the specific crossing that
// caused it gets its weight bumped (with all weights decaying slightly
// toward 1 first). A slot's priority is its (masked) domain size divided
// by the summed weight of its crossings to still-unassigned neighbors --
// lower is more urgent. Unlike plain MRV, this lets the search notice
// "this crossing region keeps blowing up" partway through and start
// prioritizing it, rather than using a fixed notion of constrainedness
// for the whole search. Word-uniqueness during search is enforced by
// masking each slot's domain against a global "used words of this length"
// bitset at read time, rather than writing exclusions into every sibling
// domain on each assignment.
//
// Those crossing weights are stored lazily-decayed (CrossingWeights,
// below), not decayed on every crossing on every wipeout: that eager
// version was an O(total grid crossings) pass paid on most nodes, fixed
// with the same trick MiniSat uses for VSIDS variable-activity bumping
// (Een & Sorensson, "An Extensible SAT-solver," SAT 2003 -- see
// docs/bibliography.md) -- see the CrossingWeights comment for the
// derivation and docs/design.md for why this one, unlike most of this
// solver's other constant-factor fixes, isn't provably behavior-
// preserving and was benchmarked like a heuristic change instead.
//
// On top of that, Solve() wraps the search in randomized restarts with a
// geometrically-growing backtrack budget, also ported from ingrid_core
// (its `find_fill`/`find_fill_for_seed`, plus the constants
// RANDOM_SLOT_WEIGHTS/RETRY_GROWTH_FACTOR and its starting max_backtracks
// of 500): an attempt that racks up too many dead ends gives up and
// retries from the root with a larger budget and a new RNG seed, but the
// *same* (already-learned) crossing weights. This is a direct response to
// Gomes, Selman & Kautz's "Boosting Combinatorial Search Through
// Randomization" (AAAI 1998): backtracking search on hard instances has a
// heavy-tailed runtime distribution -- an unlucky sequence of early
// choices can blow up to exponential cost, but that same run, replayed
// with different tie-breaks, often finishes fast -- so periodically
// abandoning a stuck attempt and reseeding beats waiting out one bad run.
// Because the backtrack budget always grows and the search is otherwise
// unchanged, this is still a complete search: an actually-unsatisfiable
// grid is still proven so, just possibly after a few budget-exceeded
// restarts rather than one pass.
//
// Each restart also leaves behind a nogood (Lecoutre, Sais, Tabary &
// Vidal, "Nogood Recording from Restarts," IJCAI 2007 -- see
// docs/bibliography.md): whenever a slot's candidate loop runs to
// genuine, complete exhaustion -- every candidate tried and undone with
// the attempt not yet aborted -- and that exhaustion is specifically what
// pushes the backtrack count over budget, the entire current assignment
// is recorded (RecordNogoodFromDeadEnd) as a combination already proven
// to be a dead end. Later restarts check, once per node
// (NogoodForbiddenWords), whether the branch slot's candidate would
// complete a recorded nogood, skipping it without re-deriving the same
// failure. This is deliberately not the same as this project's earlier,
// reverted nogood-learning attempt, which recorded from every domain
// wipeout throughout search and thereby perturbed the *same* attempt's
// own trajectory; recording only from already-exhausted, restart-
// triggering branches keeps the nogood count bounded by the restart
// count and can only ever help a *later* restart, never the one that
// produced it.
//
// The *first* attempt (attempt 0) always picks the single best dom/wdeg
// slot deterministically -- exactly the pre-restart behavior -- and only
// restarts (attempt > 0) switch SelectBranchSlot to a weighted-random pick
// among the best few. Benchmarking showed always-randomizing regresses
// grids the plain greedy choice already solves well (see
// randomize_slot_choice_ below for numbers): the point of randomization is
// to escape a demonstrably-stuck search, not to second-guess a search that
// hasn't shown any trouble yet.
//
// Word *candidate* choice within a slot deliberately stays plain
// ScoreOrder iteration, never randomized -- ingrid_core also randomizes
// word choice (RANDOM_WORD_WEIGHTS), but doing that here would fight this
// project's explicit score-quality-first goal (see the "prefers the
// higher-scored word" test) by sometimes trying a worse-scored word before
// a better one is exhausted. ingrid_core's adaptive-branching "stickiness"
// (stay on the previous slot if it's still nearly-best, to avoid
// thrashing between near-tied slots) is also not ported: it's meaningful
// in ingrid_core's iterative loop, where a slot can remain the current
// target across several word attempts, but not in this solver's design,
// where Assign() immediately collapses a chosen slot to a singleton and
// removes it from consideration -- there is no "still open" slot left to
// stick to.
//
// SolveParallel runs several independent copies of the whole scheme
// above concurrently -- a portfolio of restart sequences racing each
// other -- instead of the single sequential retry loop Solve() runs on
// its own. This is a direct, if belated, application of this project's
// own restart citation (Gomes, Selman & Kautz: backtracking runtime is
// heavy-tailed, so a *different* random run of the same search often
// finishes fast even when this one hasn't) to hardware this solver had
// simply never used: each worker gets its own private Solver instance
// (own domains, trail, crossing weights, nogoods, RNG -- nothing search-
// related is shared across threads, so there's no synchronization inside
// the hot path at all), seeded so worker 0 reproduces today's exact
// single-threaded sequence (attempt 0 deterministic, then randomized
// restarts) while every other worker randomizes from its own first
// attempt -- otherwise it would just redo worker 0's deterministic pass
// for free. A shared atomic flag, checked once per node alongside the
// existing backtrack-budget abort check, lets every other worker unwind
// within a node of whichever one finds a solution first; if every worker
// instead independently proves the grid unsatisfiable, the search is
// still complete -- SolveParallel only returns nullopt once all of them
// have. See docs/design.md for the design rationale and measured effect,
// and docs/bibliography.md for why this wasn't done sooner.
class Solver {
 public:
  Solver(const Grid& grid, const Dictionary& dict);

  // Runs constraint propagation + backtracking search. Returns the first
  // valid solution found, or nullopt if none exists (or if `cancel` is
  // set by another thread first -- see SolveParallel, which is what
  // actually needs these two parameters; a single-threaded caller can
  // ignore both).
  std::optional<Solution> Solve(uint64_t attempt_offset = 0,
                                 const std::atomic<bool>* cancel = nullptr);

  // Runs num_threads independent Solve() calls concurrently (0 means
  // std::thread::hardware_concurrency(), falling back to 1 if that can't
  // be determined) and returns as soon as any of them finds a solution,
  // or once all of them have independently proven there isn't one. See
  // the class comment above for the design.
  static ParallelSolveResult SolveParallel(const Grid& grid, const Dictionary& dict,
                                            unsigned num_threads = 0);

  const SolverStats& stats() const { return stats_; }

 private:
  struct SlotCrossing {
    int neighbor;
    int my_offset;
    int neighbor_offset;
    // Id into crossing_weights, shared by both directions of the same
    // grid crossing -- lets dom/wdeg attribute a wipeout to the specific
    // arc that caused it.
    int crossing_id;
    // grid_.SlotById(neighbor).length, precomputed once here instead of
    // re-fetched every time Propagate visits this crossing (its hottest
    // loop, run on every popped slot): `neighbor` can be any slot in the
    // grid, so grid_.SlotById(neighbor) is a cache-miss-prone random
    // access into a vector<Slot> (each Slot padded out by its own `cells`
    // vector), while crossings_by_slot_[slot] -- and so this field -- is
    // already being scanned sequentially, so it costs nothing extra to
    // have it sitting right here. See docs/design.md for the measured
    // effect.
    int neighbor_length;
  };

  struct DomainSnapshot {
    int slot;
    WordBitset domain;
  };

  struct UsedSnapshot {
    int length;
    size_t word_index;
  };

  struct Trail {
    std::vector<DomainSnapshot> domains;
    std::vector<UsedSnapshot> used;
  };

  // A recorded dead end: `pairs` (slot id, word index) together are
  // infeasible -- i.e. no solution has every one of these slots assigned
  // to that exact word simultaneously. See RecordNogoodFromDeadEnd and the
  // class comment's restart section for how these are derived (only from
  // a genuinely, completely exhausted slot -- not merely a budget cutoff)
  // and used (NogoodForbiddenWords).
  struct Nogood {
    std::vector<std::pair<int, size_t>> pairs;
  };

  // dom/wdeg's crossing weights, lazily decayed instead of eagerly.
  // Conceptually each crossing i has a weight w_i, starting at 1, updated
  // on *every* wipeout (anywhere in the grid) as
  // w_i <- 1 + (w_i - 1) * kWeightAgeFactor + [i == culprit] -- every
  // weight decays a little each time, and the crossing that caused this
  // particular wipeout also gets bumped. Applying that to every crossing
  // on every wipeout is an O(total grid crossings) pass paid on most
  // nodes of the search. Substituting x_i = w_i - 1 turns the recurrence
  // into x_i <- x_i * kWeightAgeFactor + [i == culprit] -- a plain "decay
  // everything, bump one" update, exactly the shape MiniSat's VSIDS
  // variable-activity bumping solves the same way: track x_i implicitly
  // as offset[i] * scale_, where scale_ absorbs the decay (scale_ *=
  // kWeightAgeFactor on every event, offset[culprit] += 1/scale_ on a
  // bump), so a non-culprit crossing's offset is provably unchanged by an
  // event that doesn't concern it -- Bump() becomes O(1) instead of
  // O(total crossings). scale_ shrinks every event, so it's renormalized
  // (a real O(n) pass, but only roughly once every ~2300 events -- solve
  // kWeightAgeFactor^n = kRenormalizeThreshold for n) before it could
  // underflow. See docs/design.md for the derivation check and measured
  // effect.
  //
  // Not numerically identical to the eager recurrence it replaces: the
  // eager version accumulates its own float rounding error over
  // thousands of sequential (w-1)*d operations on a bumped crossing,
  // while this scheme reaches the same value in one multiplication
  // (offset*scale) and so tracks the true mathematical value more
  // closely -- confirmed (see docs/design.md) that switching to double
  // changes nothing, i.e. the difference is the eager float32 baseline's
  // own drift, not error introduced here. Usually too small to matter,
  // but dom/wdeg's priority sort can have a near-exact tie where it picks
  // a different slot -- so, in practice, this is a search-order-affecting
  // change like any heuristic tweak, not a provably behavior-preserving
  // refactor, and was benchmarked accordingly.
  struct CrossingWeights {
    explicit CrossingWeights(size_t n) : offset(n, 0.0f) {}

    float Get(int id) const {
      return 1.0f + offset[static_cast<size_t>(id)] * scale;
    }

    void Bump(int culprit) {
      scale *= kWeightAgeFactor;
      offset[static_cast<size_t>(culprit)] += 1.0f / scale;
      if (scale < kRenormalizeThreshold) Renormalize();
    }

    void Renormalize() {
      for (float& v : offset) v *= scale;
      scale = 1.0f;
    }

    // How much a crossing weight decays toward 1 every time some *other*
    // crossing causes a wipeout -- lower prioritizes recent conflicts over
    // older ones. Value taken from rf-/ingrid_core's WEIGHT_AGE_FACTOR.
    static constexpr float kWeightAgeFactor = 0.99f;
    static constexpr float kRenormalizeThreshold = 1e-10f;

    std::vector<float> offset;
    float scale = 1.0f;
  };

  // Saves `domains[slot]` onto the trail, but only if it hasn't already
  // been saved during this `epoch` -- the first snapshot within a
  // decision level is the one that must survive to Undo, since it's the
  // only one reflecting the state before *any* of this level's changes.
  // `epoch` is a value unique to one Assign()-triggered cascade (see
  // next_save_epoch_): unlike a trail-size mark, which gets reused
  // whenever a sibling candidate at the same depth is tried after Undo
  // restores the trail back to the same size, an epoch is never reused,
  // so a plain equality check replaces what used to be a linear scan over
  // the trail looking for an existing entry for this slot.
  void SaveDomainOnce(int slot, const std::vector<WordBitset>& domains,
                       Trail& trail, uint64_t epoch) const;

  // Queue-based AC-3: seeds the propagation queue with `seed_slots` and
  // runs to a fixpoint, narrowing crossing neighbors' domains and
  // re-queueing whichever ones actually shrank. Domain changes are
  // recorded on `trail` (deduped against `epoch`, see SaveDomainOnce) so
  // the caller can undo them later. Returns false on contradiction (a
  // domain emptied), after bumping the responsible crossing's weight.
  bool Propagate(std::vector<WordBitset>& domains,
                  const std::vector<int>& seed_slots, Trail& trail,
                  uint64_t epoch, CrossingWeights& crossing_weights) const;

  // Root-only pass: once a slot's domain is forced to a single word,
  // removes that word from every other same-length slot's domain. Sets
  // `changed` if any domain was narrowed. Returns false on contradiction.
  bool EnforceUniqueWordsOnce(std::vector<WordBitset>& domains,
                               bool& changed) const;

  // Sum of crossing_weights over this slot's crossings to still-
  // unassigned neighbors (the wdeg of dom/wdeg). Falls back to 1 if the
  // slot has no such crossings, so priority reduces to plain domain size.
  float SlotWeight(int slot, const CrossingWeights& crossing_weights,
                    const std::vector<bool>& assigned) const;

  // Index (into slots_by_component_) of the lowest-numbered connected
  // component that still has an unassigned slot, or -1 if none do.
  // O(component count), not O(slot count) -- component_remaining_ is
  // maintained incrementally by Assign/Undo specifically so this stays
  // cheap even late in a large multi-component search.
  int ActiveComponent() const;

  // dom/wdeg branching, restricted to the current *active component* (see
  // slots_by_component_ below): smallest (masked domain size / weighted
  // degree), i.e. most urgent first, is the top candidate within that
  // component -- but rather than always taking that single best slot, a
  // weighted-random choice is made among the best few (see
  // kRandomSlotWeights in solver.cpp), so that different restart attempts
  // (see Solve()) actually explore different branch orders instead of
  // deterministically replaying the same one. A domain that comes up
  // empty is deliberately not skipped -- it looks maximally urgent, tends
  // to get selected, and the caller's candidate loop then finds no
  // matches and backtracks, which is how contradictions surface. Returns
  // -1 once every slot is assigned.
  //
  // Note this relies on `assigned` rather than inferring "settled" from
  // domain size alone: once a slot is assigned, its own chosen word is
  // marked used, which would make its own masked domain look empty (self-
  // exclusion) if re-examined -- that's a false contradiction, not a real
  // one, so assigned slots must be skipped explicitly instead.
  // `out_domain_count`, when non-null, receives the chosen slot's popcount
  // of (domain & ~used) -- already computed here while scoring every
  // candidate in the component, so the caller (Backtrack, choosing between
  // its two candidate-iteration strategies) can reuse it instead of paying
  // for the same fused AndNot-count a second time.
  int SelectBranchSlot(const std::vector<WordBitset>& domains,
                        const std::vector<WordBitset>& used_by_length,
                        const std::vector<bool>& assigned,
                        const CrossingWeights& crossing_weights,
                        size_t* out_domain_count = nullptr) const;

  // Assigns `slot` to word `word_index`, marks the word used, and runs
  // Propagate from `slot` to cascade the consequences. Returns false on
  // contradiction; the caller must still Undo regardless. Generates its
  // own fresh epoch (see next_save_epoch_) for this call's SaveDomainOnce
  // dedup, shared between the direct snapshot of `slot` itself and
  // everything the resulting Propagate cascade touches.
  bool Assign(int slot, size_t word_index, std::vector<WordBitset>& domains,
              std::vector<WordBitset>& used_by_length,
              std::vector<bool>& assigned, Trail& trail,
              CrossingWeights& crossing_weights) const;

  // Rolls `domains`/`used_by_length`/`assigned` back to the given trail
  // marks for the given slot.
  void Undo(int slot, std::vector<WordBitset>& domains,
            std::vector<WordBitset>& used_by_length,
            std::vector<bool>& assigned, Trail& trail, size_t domain_mark,
            size_t used_mark) const;

  std::optional<Solution> Backtrack(std::vector<WordBitset>& domains,
                                     std::vector<WordBitset>& used_by_length,
                                     std::vector<bool>& assigned, Trail& trail,
                                     CrossingWeights& crossing_weights);

  // Records a nogood from the current assignment: every currently-assigned
  // slot's (slot, word) pair, taken together, is infeasible. Sound to call
  // only when `slot` (whose candidate loop just ran to completion, every
  // candidate genuinely tried and undone with aborted_ still false at each
  // step -- never merely cut short by the backtrack budget) has no
  // remaining valid word given exactly this ancestor assignment. See the
  // Solver class comment's restart section for why this can't fire for a
  // budget-truncated branch, and why using the full current assignment
  // (rather than only this decision's true ancestors) is still sound, just
  // more conservative.
  void RecordNogoodFromDeadEnd(const std::vector<WordBitset>& domains,
                                const std::vector<bool>& assigned);

  // Words forbidden for `slot` by any recorded nogood whose *other* pairs
  // are all already satisfied by the current assignment -- i.e. assigning
  // one of these words to `slot` right now would complete a combination
  // already proven, in an earlier restart, to be a dead end. Returns
  // nullptr if none apply (the common case once no nogood mentions `slot`
  // at all).
  const WordBitset* NogoodForbiddenWords(int slot,
                                          const std::vector<WordBitset>& domains,
                                          const std::vector<bool>& assigned) const;

  Solution ExtractSolution(const std::vector<WordBitset>& domains) const;

  const Grid& grid_;
  const Dictionary& dict_;
  SolverStats stats_;
  // Longest slot in the grid, computed once in the constructor and reused
  // everywhere a per-length scratch vector needs sizing (its own
  // scratch buffers below, and Solve()'s used_by_length) -- avoids
  // re-deriving it from slots_by_length_ a second time.
  int max_length_ = 0;
  // slot_length_[slot_id] -- every slot's length, populated once in the
  // constructor. Every hot-path caller that needs a slot's length
  // (SaveDomainOnce, Propagate, SelectBranchSlot, Assign, Undo,
  // Backtrack, NogoodForbiddenWords -- i.e. most of them) used to fetch
  // it via grid_.SlotById(id).length, a random access into a
  // vector<Slot> (each Slot padded out by its own `cells` member) --
  // this is a small, dense array instead, cheaper to keep in cache for
  // the same reason SlotCrossing::neighbor_length is (see docs/design.md
  // for the measured effect of both).
  std::vector<int> slot_length_;
  // Slot ids grouped by length -- only same-length slots can ever collide
  // on the same word, so this scopes the uniqueness checks.
  std::unordered_map<int, std::vector<int>> slots_by_length_;
  // crossings_by_slot_[slot_id] -- every other slot it crosses, and the
  // offset within each side of the crossing cell.
  std::vector<std::vector<SlotCrossing>> crossings_by_slot_;

  // Connected components of the slot-crossing graph (BFS over
  // crossings_by_slot_, computed once in the constructor -- see the
  // class comment's discussion of Dechter's "non-separable components").
  // Two slots in different components share no crossing, directly or
  // transitively, so nothing about one can ever affect the other's
  // *word-matching* constraints -- only the grid-wide no-duplicate-words
  // rule still couples them. slots_by_component_[c] lists component c's
  // slot ids; component_remaining_[c] is how many of them are still
  // unassigned in the *current* search attempt (decremented by Assign,
  // incremented by Undo, reset at the top of each attempt in Solve()).
  // SelectBranchSlot only ever offers candidates from the lowest-indexed
  // component with any left -- fully settling (or proving impossible)
  // one component before starting the next. This is free (identical to
  // the old unrestricted behavior) when the grid is a single component,
  // which real, well-interlocked crosswords usually are; it shrinks the
  // candidate scan for grids that do have independent regions, e.g.
  // benchmarks/grids/synthetic/disconnected_15x15.txt.
  std::vector<std::vector<int>> slots_by_component_;
  std::vector<int> component_of_slot_;
  mutable std::vector<int> component_remaining_;
  // One scratch WordBitset per word length, reused as Propagate's `filter`
  // instead of heap-allocating a fresh one on every crossing check --
  // profiling showed WordBitset construction/destruction was a real cost
  // in this hot loop. Indexed directly by length (like Dictionary's
  // internals) rather than an unordered_map, since this is looked up once
  // per crossing inside Propagate's innermost loop and a hash + bucket
  // lookup there was itself a measurable cost. Sized once in the
  // constructor; ClearAll() resets without reallocating. Mutable for the
  // same reason as `rng_`.
  mutable std::vector<WordBitset> filter_scratch_by_length_;

  // Propagate's queue membership, kept as a persistent scratch buffer
  // (sized once to the slot count) instead of a freshly-allocated
  // vector<bool> on every call -- Propagate runs once per node (i.e. very
  // often), and profiling showed the per-call allocation was a real cost.
  // Every entry is false again by the time Propagate returns (queue_touched_
  // below records exactly which entries were ever set true, so returning
  // early on a contradiction can still reset just those instead of the
  // whole vector). Mutable for the same reason as `rng_`.
  mutable std::vector<bool> in_queue_scratch_;
  mutable std::vector<int> queue_touched_scratch_;
  // Propagate's cache of each queued slot's domain popcount -- see the
  // comment where it's used in Propagate for why this is always valid
  // for as long as a slot stays queued.
  mutable std::vector<size_t> queued_count_scratch_;
  // Propagate's flat, unordered list of currently-queued slot ids, used
  // by the min-domain pop so it scans only actually-queued slots instead
  // of every slot in the grid. See the comment where it's used in
  // Propagate for why this stays unsorted (no maintenance cost) unlike an
  // earlier, reverted sorted-vector attempt at the same idea.
  mutable std::vector<int> active_queue_scratch_;

  // Propagate's "which words are still in this slot's domain" list for the
  // direct-lookup path, reused across calls (via WordBitset::AppendSetBits)
  // instead of a fresh std::vector<size_t> per popped queue slot.
  mutable std::vector<size_t> slot_candidates_scratch_;

  // SelectBranchSlot's (priority, slot id, domain_count) candidate list,
  // reused across calls instead of a fresh vector on every single
  // branching decision. Default tuple ordering sorts by priority first,
  // slot id second (id ties never actually occur, since ids are unique) --
  // domain_count is carried along purely for the caller to retrieve, never
  // itself compared.
  mutable std::vector<std::tuple<float, int, size_t>> branch_candidates_scratch_;

  // SaveDomainOnce's O(1) replacement for its old linear trail scan:
  // last_saved_epoch_[slot] holds the epoch (see next_save_epoch_) during
  // which `slot` was last snapshotted, or 0 (never a valid epoch) if
  // never. Each Assign()-triggered cascade (and each root-propagation
  // pass in Solve()) draws a fresh, never-repeated epoch from
  // next_save_epoch_, so "already saved during the current epoch" is a
  // single equality check instead of scanning the trail for an existing
  // entry -- this matters because a *trail-size* mark, unlike an epoch,
  // gets reused whenever Undo restores the trail back to the same size
  // for a sibling candidate at the same search depth, which would make a
  // plain "already saved, ever" flag incorrectly skip a snapshot that a
  // deeper, already-undone cascade also needed at that same trail size.
  mutable std::vector<uint64_t> last_saved_epoch_;
  mutable uint64_t next_save_epoch_ = 1;

  // Recycle pool for trail domain snapshots, one stack per length (a
  // WordBitset's buffer size is fixed by its length, so a buffer freed by
  // Undo for one length can't be reused for another). Profiling showed
  // SaveDomainOnce's heap allocation (copying a slot's domain onto the
  // trail) and Undo's corresponding free (releasing the domain state a
  // restored snapshot just overwrote) were a real, repeated cost -- this
  // pair runs on every domain a propagation cascade actually narrows, very
  // often. Undo pushes the about-to-be-discarded domain here instead of
  // letting the move-assignment that replaces it free it; SaveDomainOnce
  // pops a buffer from here and copies into it in place, instead of
  // heap-allocating a fresh one, whenever one is available.
  mutable std::vector<std::vector<WordBitset>> snapshot_pool_by_length_;

  // Nogoods recorded from restarts (Lecoutre, Sais, Tabary & Vidal,
  // "Nogood Recording from Restarts," IJCAI 2007 -- see
  // docs/bibliography.md): unlike this project's earlier, reverted
  // attempt at nogood learning (which recorded from *every* domain
  // wipeout throughout search), these are recorded only from a slot whose
  // candidate loop genuinely, completely exhausted -- never from a branch
  // merely cut short by the backtrack budget -- and persist across
  // restarts *within* one Solve() call (that's the point: stop a later
  // attempt from redoing a dead end an earlier one already fully proved).
  // nogoods_by_slot_ maps a slot id to the indices (into nogoods_) of
  // every nogood mentioning it, so NogoodForbiddenWords only has to check
  // nogoods actually relevant to the slot being branched on.
  mutable std::vector<Nogood> nogoods_;
  mutable std::unordered_map<int, std::vector<int>> nogoods_by_slot_;
  // Scratch bitset (one per length) for NogoodForbiddenWords' result,
  // reused across calls like filter_scratch_by_length_.
  mutable std::vector<WordBitset> nogood_forbidden_scratch_by_length_;

  // Backtrack's scratch bitset (one per length) for its narrow-domain
  // candidate path: "this slot's domain, minus used words, minus any
  // nogood-forbidden words" is built here in place instead of allocating a
  // fresh WordBitset per branching node. Only ever read from immediately
  // after being written, before any recursive Backtrack call that could
  // reuse the same length's buffer -- unlike the *extracted index list*
  // (a local variable in Backtrack, not scratch state), this bitset itself
  // never needs to survive across a recursive call.
  mutable std::vector<WordBitset> candidate_scratch_by_length_;

  // Restart-related state, all reset at the top of each attempt inside
  // Solve()'s retry loop (see the class comment above for the design this
  // is drawn from). `rng_` is mutable because SelectBranchSlot, which
  // draws from it, is logically a const query over the current search
  // state.
  mutable std::mt19937_64 rng_{0};
  uint64_t attempt_backtracks_ = 0;
  uint64_t attempt_backtrack_limit_ = 0;
  bool aborted_ = false;
  // Set for the duration of one Solve() call from that call's `cancel`
  // parameter (null for an ordinary single-threaded call). Checked in
  // Backtrack alongside aborted_, once per node -- SolveParallel's other
  // workers use this to unwind promptly once one of them finds a
  // solution, without any synchronization inside the search itself.
  const std::atomic<bool>* cancel_ = nullptr;
  // Whether SelectBranchSlot should weighted-randomly pick among the top
  // few slots (true on restarts) or deterministically take the single
  // best one (false on the first attempt). Benchmarking showed always
  // randomizing regresses grids that the plain greedy dom/wdeg choice
  // already solves well (e.g. sample_7x7.txt: 22 nodes/0 backtracks
  // greedy vs. 2597 nodes/93 backtracks always-randomized) -- the
  // diversity is only worth its variance cost once the greedy attempt has
  // already proven insufficient.
  bool randomize_slot_choice_ = false;
};

}  // namespace xfill
