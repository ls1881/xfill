#include "xfill/grid.hpp"

#include <stdexcept>

namespace xfill {

Grid::Grid(int width, int height)
    : width_(width), height_(height), blocked_(width * height, false) {}

Grid Grid::FromSpec(const std::vector<std::string>& rows) {
  if (rows.empty()) {
    throw std::invalid_argument("grid spec must have at least one row");
  }
  int height = static_cast<int>(rows.size());
  int width = static_cast<int>(rows[0].size());
  Grid grid(width, height);

  for (int r = 0; r < height; ++r) {
    if (static_cast<int>(rows[r].size()) != width) {
      throw std::invalid_argument("all rows must have equal width");
    }
    for (int c = 0; c < width; ++c) {
      grid.blocked_[r * width + c] = (rows[r][c] == '#');
    }
  }

  grid.ComputeSlots();
  grid.ComputeCrossings();
  return grid;
}

void Grid::ComputeSlots() {
  // TODO: scan rows for across slots, columns for down slots.
  // A slot starts at a cell that (a) is open and (b) has a blocked/edge
  // cell immediately before it in the given direction, and has length >= 2.
}

void Grid::ComputeCrossings() {
  // TODO: for every cell covered by exactly one across slot and one down
  // slot, record a Crossing with the offset into each.
}

}  // namespace xfill
