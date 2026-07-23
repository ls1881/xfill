#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>

#include "xfill/grid.hpp"
#include "xfill/solver.hpp"

namespace {
xfill::Dictionary WriteAndLoadDict(const std::string& path,
                                    const std::vector<std::string>& lines) {
  std::ofstream out(path);
  for (const auto& line : lines) out << line << "\n";
  out.close();
  auto dict = xfill::Dictionary::LoadFromFile(path);
  std::remove(path.c_str());
  return dict;
}
}  // namespace

TEST_CASE("Solver fills a single open slot with no crossings") {
  auto grid = xfill::Grid::FromSpec({"..."});
  auto dict = WriteAndLoadDict("test_single_slot.dict", {"CAT;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE(solution.has_value());
  REQUIRE(solution->assignment.at(0) == "CAT");
}

TEST_CASE("Solver returns nullopt when no word of the needed length exists") {
  auto grid = xfill::Grid::FromSpec({"...."});
  auto dict = WriteAndLoadDict("test_no_solution.dict", {"CAT;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE_FALSE(solution.has_value());
}

TEST_CASE("Solver respects crossing constraints on a 2x2 grid") {
  auto grid = xfill::Grid::FromSpec({"..", ".."});
  // TO / OK is the only mutually consistent word square from these two
  // words: OK / TO crossed the other way would require "OT" and "TK",
  // neither of which is in the dictionary.
  auto dict = WriteAndLoadDict("test_crossing.dict", {"TO;10", "OK;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE(solution.has_value());

  // Slot ids: across slots assigned first in row order (0, 1), then down
  // slots in column order (2, 3).
  REQUIRE(solution->assignment.at(0) == "TO");  // across, row 0
  REQUIRE(solution->assignment.at(1) == "OK");  // across, row 1
  REQUIRE(solution->assignment.at(2) == "TO");  // down, col 0
  REQUIRE(solution->assignment.at(3) == "OK");  // down, col 1
}
