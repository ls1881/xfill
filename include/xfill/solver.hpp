#pragma once

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
class Solver {
 public:
  Solver(const Grid& grid, const Dictionary& dict);

  // Runs constraint propagation + backtracking search.
  // Returns the first valid solution found, or nullopt if none exists.
  std::optional<Solution> Solve();

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

  // Saves `domains[slot]` onto the trail, but only if it hasn't already
  // been saved since `level_mark` -- the first snapshot within a decision
  // level is the one that must survive to Undo, since it's the only one
  // reflecting the state before *any* of this level's changes.
  void SaveDomainOnce(int slot, const std::vector<WordBitset>& domains,
                       Trail& trail, size_t level_mark) const;

  // Decays every crossing weight toward 1 (keeping WEIGHT_AGE_FACTOR of
  // its excess) and bumps `culprit`'s weight by 1 -- called once per
  // propagation failure, attributing the wipeout to the arc that caused
  // it while letting older conflicts fade.
  void BumpCrossingWeight(std::vector<float>& crossing_weights,
                          int culprit) const;

  // Queue-based AC-3: seeds the propagation queue with `seed_slots` and
  // runs to a fixpoint, narrowing crossing neighbors' domains and
  // re-queueing whichever ones actually shrank. Domain changes are
  // recorded on `trail` (deduped against `level_mark`) so the caller can
  // undo them later. Returns false on contradiction (a domain emptied),
  // after bumping the responsible crossing's weight.
  bool Propagate(std::vector<WordBitset>& domains,
                  const std::vector<int>& seed_slots, Trail& trail,
                  size_t level_mark, std::vector<float>& crossing_weights) const;

  // Root-only pass: once a slot's domain is forced to a single word,
  // removes that word from every other same-length slot's domain. Sets
  // `changed` if any domain was narrowed. Returns false on contradiction.
  bool EnforceUniqueWordsOnce(std::vector<WordBitset>& domains,
                               bool& changed) const;

  // Sum of crossing_weights over this slot's crossings to still-
  // unassigned neighbors (the wdeg of dom/wdeg). Falls back to 1 if the
  // slot has no such crossings, so priority reduces to plain domain size.
  float SlotWeight(int slot, const std::vector<float>& crossing_weights,
                    const std::vector<bool>& assigned) const;

  // dom/wdeg branching over not-yet-assigned slots: smallest (masked
  // domain size / weighted degree), i.e. most urgent first, is the top
  // candidate -- but rather than always taking that single best slot, a
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
  int SelectBranchSlot(const std::vector<WordBitset>& domains,
                        const std::vector<WordBitset>& used_by_length,
                        const std::vector<bool>& assigned,
                        const std::vector<float>& crossing_weights) const;

  // Assigns `slot` to word `word_index`, marks the word used, and runs
  // Propagate from `slot` to cascade the consequences. Returns false on
  // contradiction; the caller must still Undo regardless.
  bool Assign(int slot, size_t word_index, std::vector<WordBitset>& domains,
              std::vector<WordBitset>& used_by_length,
              std::vector<bool>& assigned, Trail& trail, size_t level_mark,
              std::vector<float>& crossing_weights) const;

  // Rolls `domains`/`used_by_length`/`assigned` back to the given trail
  // marks for the given slot.
  void Undo(int slot, std::vector<WordBitset>& domains,
            std::vector<WordBitset>& used_by_length,
            std::vector<bool>& assigned, Trail& trail, size_t domain_mark,
            size_t used_mark) const;

  std::optional<Solution> Backtrack(std::vector<WordBitset>& domains,
                                     std::vector<WordBitset>& used_by_length,
                                     std::vector<bool>& assigned, Trail& trail,
                                     std::vector<float>& crossing_weights);

  Solution ExtractSolution(const std::vector<WordBitset>& domains) const;

  const Grid& grid_;
  const Dictionary& dict_;
  SolverStats stats_;
  // Slot ids grouped by length -- only same-length slots can ever collide
  // on the same word, so this scopes the uniqueness checks.
  std::unordered_map<int, std::vector<int>> slots_by_length_;
  // crossings_by_slot_[slot_id] -- every other slot it crosses, and the
  // offset within each side of the crossing cell.
  std::vector<std::vector<SlotCrossing>> crossings_by_slot_;
  // One scratch WordBitset per word length, reused as Propagate's `filter`
  // instead of heap-allocating a fresh one on every crossing check --
  // profiling showed WordBitset construction/destruction was a real cost
  // in this hot loop. Sized once in the constructor; ClearAll() resets
  // without reallocating. Mutable for the same reason as `rng_`.
  mutable std::unordered_map<int, WordBitset> filter_scratch_by_length_;

  // Restart-related state, all reset at the top of each attempt inside
  // Solve()'s retry loop (see the class comment above for the design this
  // is drawn from). `rng_` is mutable because SelectBranchSlot, which
  // draws from it, is logically a const query over the current search
  // state.
  mutable std::mt19937_64 rng_{0};
  uint64_t attempt_backtracks_ = 0;
  uint64_t attempt_backtrack_limit_ = 0;
  bool aborted_ = false;
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
