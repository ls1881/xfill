#pragma once

#include <optional>
#include <unordered_map>

#include "xfill/dictionary.hpp"
#include "xfill/grid.hpp"

namespace xfill {

struct Solution {
  // slot id -> chosen word
  std::unordered_map<int, std::string> assignment;
};

struct SolverStats {
  uint64_t nodes = 0;
  uint64_t backtracks = 0;
  uint64_t propagation_steps = 0;
};

class Solver {
 public:
  Solver(const Grid& grid, const Dictionary& dict);

  // Runs constraint propagation + backtracking search.
  // Returns the first valid solution found, or nullopt if none exists.
  std::optional<Solution> Solve();

  const SolverStats& stats() const { return stats_; }

 private:
  const Grid& grid_;
  const Dictionary& dict_;
  SolverStats stats_;

  // TODO: per-slot domains (WordBitset), propagate(), branch(), etc.
};

}  // namespace xfill
