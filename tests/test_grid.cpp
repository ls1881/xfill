#include <catch2/catch_test_macros.hpp>

#include "xfill/grid.hpp"

TEST_CASE("Grid::FromSpec builds a grid with correct dimensions") {
  auto grid = xfill::Grid::FromSpec({"...", "...", "..."});
  REQUIRE(grid.width() == 3);
  REQUIRE(grid.height() == 3);
}

TEST_CASE("A fully open grid has one across and one down slot per row/column") {
  auto grid = xfill::Grid::FromSpec({"...", "...", "..."});
  // 3 across (one per row) + 3 down (one per column) = 6 total.
  REQUIRE(grid.slots().size() == 6);
}

TEST_CASE("A block splits a row into two shorter across slots") {
  auto grid = xfill::Grid::FromSpec({
      "..#..",
      ".....",
      ".....",
      ".....",
      ".....",
  });
  int row0_across_count = 0;
  for (const auto& slot : grid.slots()) {
    if (slot.dir == xfill::Direction::Across && slot.row == 0) {
      row0_across_count++;
    }
  }
  REQUIRE(row0_across_count == 2);
}

TEST_CASE("A single blocked cell in the middle of a long row is dropped, not orphaned") {
  // ".#." would leave two length-1 runs, which shouldn't become slots.
  auto grid = xfill::Grid::FromSpec({".#."});
  REQUIRE(grid.slots().empty());
}

TEST_CASE("Crossings are recorded for every cell shared by an across and down slot") {
  auto grid = xfill::Grid::FromSpec({"...", "...", "..."});
  // Every cell in a fully open 3x3 grid belongs to both an across and a
  // down slot, so there should be exactly one crossing per cell.
  REQUIRE(grid.crossings().size() == 9);
}
