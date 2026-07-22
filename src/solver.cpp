#include "xfill/solver.hpp"

namespace xfill {

Solver::Solver(const Grid& grid, const Dictionary& dict)
    : grid_(grid), dict_(dict) {}

std::optional<Solution> Solver::Solve() {
  // TODO:
  //  1. Initialize per-slot domains from dictionary word lists.
  //  2. Run initial AC-3 style propagation across all crossings.
  //  3. Loop: pick branch cell (MRV / SoCDP-style heuristic), try each
  //     viable letter, propagate, recurse; backtrack on contradiction.
  //  4. Return assignment once every slot has exactly one candidate.
  return std::nullopt;
}

}  // namespace xfill
