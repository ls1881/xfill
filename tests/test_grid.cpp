#include <catch2/catch_test_macros.hpp>

#include "xfill/grid.hpp"

TEST_CASE("Grid::FromSpec builds a grid with correct dimensions") {
  auto grid = xfill::Grid::FromSpec({"...", "...", "..."});
  REQUIRE(grid.width() == 3);
  REQUIRE(grid.height() == 3);
}

// TODO: once ComputeSlots/ComputeCrossings are implemented, add cases for:
//  - a simple open 3x3 grid's slot count
//  - a grid with a block splitting a row into two slots
//  - crossing offsets matching expected cell positions
