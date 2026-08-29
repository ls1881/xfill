#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>

#include "xfill/grid.hpp"

namespace {

const xfill::Slot& FindSlot(const xfill::Grid& grid, xfill::Direction dir, int row, int col) {
  for (const auto& slot : grid.slots()) {
    if (slot.dir == dir && slot.row == row && slot.col == col) return slot;
  }
  throw std::runtime_error("no matching slot found");
}

}  // namespace

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

TEST_CASE("A pre-filled letter is an ordinary open cell, not a block") {
  auto grid = xfill::Grid::FromSpec({"A..", "...", "..."});
  REQUIRE_FALSE(grid.IsBlocked(0, 0));
  REQUIRE(grid.slots().size() == 6);  // same shape as a fully-open 3x3
}

TEST_CASE("Grid::PrefilledLetter reports the seeded letter, uppercased") {
  auto grid = xfill::Grid::FromSpec({"a..", ".B.", "..."});
  REQUIRE(grid.PrefilledLetter(0, 0) == 'A');
  REQUIRE(grid.PrefilledLetter(1, 1) == 'B');
  REQUIRE(grid.PrefilledLetter(2, 2) == '\0');
}

TEST_CASE("A blocked cell is never reported as pre-filled even if it looks like one") {
  auto grid = xfill::Grid::FromSpec({"#.."});
  REQUIRE(grid.IsBlocked(0, 0));
  REQUIRE(grid.PrefilledLetter(0, 0) == '\0');
}

TEST_CASE("A rebus cell expands its slots' real length beyond their physical cell count") {
  // Fully open 3x3, rebus "AD" at (0,0) -- both the row-0 across slot and
  // the column-0 down slot cover it, so both should gain 1 extra
  // character of real length (2-char cell instead of 1) over their
  // physical 3-cell run.
  auto grid = xfill::Grid::FromSpec({"A..", "...", "..."}, {{0, 0, "AD"}});
  const xfill::Slot& across = FindSlot(grid, xfill::Direction::Across, 0, 0);
  const xfill::Slot& down = FindSlot(grid, xfill::Direction::Down, 0, 0);

  REQUIRE(across.cells.size() == 3);
  REQUIRE(across.length == 4);
  REQUIRE(across.cell_lengths == std::vector<int>{2, 1, 1});
  REQUIRE(across.cell_char_start == std::vector<int>{0, 2, 3});

  REQUIRE(down.cells.size() == 3);
  REQUIRE(down.length == 4);
  REQUIRE(down.cell_lengths == std::vector<int>{2, 1, 1});
  REQUIRE(down.cell_char_start == std::vector<int>{0, 2, 3});

  REQUIRE(grid.RebusContent(0, 0) == "AD");
  REQUIRE(grid.PrefilledLetter(0, 0) == 'A');  // still the first-letter stand-in
}

TEST_CASE("A slot with no rebus cell keeps length == cells.size(), same as before this existed") {
  auto grid = xfill::Grid::FromSpec({"...", "...", "..."});
  for (const auto& slot : grid.slots()) {
    REQUIRE(slot.length == static_cast<int>(slot.cells.size()));
    for (size_t k = 0; k < slot.cells.size(); ++k) {
      REQUIRE(slot.cell_lengths[k] == 1);
      REQUIRE(slot.cell_char_start[k] == static_cast<int>(k));
    }
  }
}

TEST_CASE("A crossing at a rebus cell decomposes into one Crossing per character") {
  auto grid = xfill::Grid::FromSpec({"A..", "...", "..."}, {{0, 0, "AD"}});
  // A fully open 3x3 grid has 9 crossing cells; the rebus one at (0,0)
  // contributes 2 Crossings instead of 1, so the total grows by exactly 1.
  REQUIRE(grid.crossings().size() == 10);

  const xfill::Slot& across = FindSlot(grid, xfill::Direction::Across, 0, 0);
  const xfill::Slot& down = FindSlot(grid, xfill::Direction::Down, 0, 0);
  std::vector<std::pair<int, int>> offsets_at_rebus;
  for (const auto& crossing : grid.crossings()) {
    if (crossing.slot_a == across.id && crossing.slot_b == down.id) {
      offsets_at_rebus.push_back({crossing.offset_a, crossing.offset_b});
    }
  }
  REQUIRE(offsets_at_rebus.size() == 2);
  REQUIRE(offsets_at_rebus[0] == std::pair<int, int>{0, 0});
  REQUIRE(offsets_at_rebus[1] == std::pair<int, int>{1, 1});
}

TEST_CASE("Grid::FromSpec rejects rebus content that isn't pure A-Z") {
  REQUIRE_THROWS_AS(xfill::Grid::FromSpec({"A.."}, {{0, 0, "A1"}}), std::invalid_argument);
}

TEST_CASE("Grid::FromSpec rejects a rebus entry out of bounds") {
  REQUIRE_THROWS_AS(xfill::Grid::FromSpec({"A.."}, {{5, 5, "AD"}}), std::invalid_argument);
}

TEST_CASE("Grid::FromSpec rejects a rebus entry at a blocked cell") {
  REQUIRE_THROWS_AS(xfill::Grid::FromSpec({"#.."}, {{0, 0, "AD"}}), std::invalid_argument);
}

TEST_CASE("Grid::FromSpec rejects a rebus entry whose first letter doesn't match the row") {
  // The row says this cell is 'A', but the rebus content starts with 'Z'.
  REQUIRE_THROWS_AS(xfill::Grid::FromSpec({"A.."}, {{0, 0, "ZD"}}), std::invalid_argument);
}

TEST_CASE("Grid::FromFile parses an optional trailing rebus section") {
  const std::string path = "test_rebus_grid_spec.txt";
  {
    std::ofstream out(path);
    out << "A..\n...\n...\n\n0,0:AD\n";
  }
  auto grid = xfill::Grid::FromFile(path);
  std::remove(path.c_str());

  REQUIRE(grid.width() == 3);
  REQUIRE(grid.height() == 3);
  REQUIRE(grid.RebusContent(0, 0) == "AD");
  const xfill::Slot& across = FindSlot(grid, xfill::Direction::Across, 0, 0);
  REQUIRE(across.length == 4);
}

TEST_CASE("Grid::FromFile with no rebus section behaves exactly as before") {
  const std::string path = "test_no_rebus_grid_spec.txt";
  {
    std::ofstream out(path);
    out << "...\n...\n...\n";
  }
  auto grid = xfill::Grid::FromFile(path);
  std::remove(path.c_str());

  REQUIRE(grid.width() == 3);
  REQUIRE(grid.height() == 3);
  REQUIRE(grid.slots().size() == 6);
  REQUIRE(grid.crossings().size() == 9);
}
