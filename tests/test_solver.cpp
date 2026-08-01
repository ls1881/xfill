#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"
#include "xfill/grid.hpp"
#include "xfill/solver.hpp"

using xfill_test::WriteAndLoadDict;

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
  // AT/NO crossing to AN/TO is the unique no-duplicate-words completion:
  // all four words in the dictionary get used exactly once, matching the
  // crossing letters. (A two-word dictionary like TO/OK would force TO to
  // be reused across and down, which the no-duplicates rule forbids.)
  auto dict = WriteAndLoadDict("test_crossing.dict",
                                {"AT;10", "NO;10", "AN;10", "TO;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE(solution.has_value());

  // Slot ids: across slots assigned first in row order (0, 1), then down
  // slots in column order (2, 3).
  REQUIRE(solution->assignment.at(0) == "AT");  // across, row 0
  REQUIRE(solution->assignment.at(1) == "NO");  // across, row 1
  REQUIRE(solution->assignment.at(2) == "AN");  // down, col 0
  REQUIRE(solution->assignment.at(3) == "TO");  // down, col 1
}

TEST_CASE("Solver refuses to reuse the same word for two different slots") {
  // Two non-crossing 3-letter slots but only one 3-letter word in the
  // dictionary -- a fill would require using "CAT" twice, which the
  // no-duplicate-words rule forbids, so no solution should be found.
  auto grid = xfill::Grid::FromSpec({"...", "###", "..."});
  auto dict = WriteAndLoadDict("test_no_dupe.dict", {"CAT;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE_FALSE(solution.has_value());
}

TEST_CASE("Solver prefers the higher-scored word among equally valid candidates") {
  auto grid = xfill::Grid::FromSpec({"..."});
  // Both words are otherwise valid fills for an unconstrained 3-letter
  // slot; the solver should pick the higher-scored one first rather than
  // whichever happens to load first from the dictionary file.
  auto dict = WriteAndLoadDict("test_score_pref.dict", {"ZZZ;10", "CAT;50"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE(solution.has_value());
  REQUIRE(solution->assignment.at(0) == "CAT");
}

TEST_CASE("Solver reports no solution for an isolated slot with no matching word length") {
  // Regression test: a slot with no crossings must still be checked for
  // an empty domain, or the solver falsely reports success and crashes
  // trying to extract a word from an empty candidate list.
  auto grid = xfill::Grid::FromSpec({"....."});
  auto dict = WriteAndLoadDict("test_isolated_empty.dict", {"CAT;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE_FALSE(solution.has_value());
}
