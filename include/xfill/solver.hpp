#pragma once

#include <cstdint>
#include <optional>
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
  // domain size / weighted degree), i.e. most urgent first. A domain that
  // comes up empty is deliberately not skipped -- it looks maximally
  // urgent, gets selected, and the caller's candidate loop then finds no
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
};

}  // namespace xfill
