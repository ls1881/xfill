#pragma once

#include "xfill/grid.hpp"

namespace xfill {

// Selects which cell to branch on next, given current domain sizes.
// Starting point: minimum-remaining-values (MRV). A SoCDP-style heuristic
// (sum of crossing domain products) can replace this once MRV is working
// end-to-end and benchmarked as a baseline.
class BranchingHeuristic {
 public:
  virtual ~BranchingHeuristic() = default;
  virtual int SelectCell(const Grid& grid) = 0;
};

class MrvHeuristic : public BranchingHeuristic {
 public:
  int SelectCell(const Grid& grid) override;
};

}  // namespace xfill
