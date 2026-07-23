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

// Correctness-first CSP solver: full AC-3 style propagation to a fixpoint
// after every guess, MRV branching, full-copy backtracking. No performance
// optimizations yet (see docs/design.md roadmap) -- this establishes a
// baseline to benchmark future changes against.
class Solver {
 public:
  Solver(const Grid& grid, const Dictionary& dict);

  // Runs constraint propagation + backtracking search.
  // Returns the first valid solution found, or nullopt if none exists.
  std::optional<Solution> Solve();

  const SolverStats& stats() const { return stats_; }

 private:
  // Propagates every crossing constraint to a fixpoint. Returns false if
  // any slot's domain becomes empty (contradiction).
  bool Propagate(std::vector<WordBitset>& domains) const;

  // MRV: returns the slot id with the smallest domain of size > 1, or -1
  // if every slot already has exactly one remaining candidate.
  int SelectBranchSlot(const std::vector<WordBitset>& domains) const;

  std::optional<Solution> Backtrack(std::vector<WordBitset> domains);

  Solution ExtractSolution(const std::vector<WordBitset>& domains) const;

  const Grid& grid_;
  const Dictionary& dict_;
  SolverStats stats_;
};

}  // namespace xfill
