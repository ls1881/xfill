#include <catch2/catch_test_macros.hpp>

#include <mutex>
#include <vector>

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

TEST_CASE("A pre-filled letter narrows the slot to only matching words") {
  // Both CAT and DOG fit an unconstrained 3-letter slot; seeding position
  // 0 with 'D' must rule out CAT even though it scores higher.
  auto grid = xfill::Grid::FromSpec({"D.."});
  auto dict = WriteAndLoadDict("test_prefill_letter.dict", {"CAT;50", "DOG;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE(solution.has_value());
  REQUIRE(solution->assignment.at(0) == "DOG");
}

TEST_CASE("A pre-filled letter with no matching word reports no solution") {
  auto grid = xfill::Grid::FromSpec({"Z.."});
  auto dict = WriteAndLoadDict("test_prefill_impossible.dict", {"CAT;50", "DOG;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE_FALSE(solution.has_value());
}

TEST_CASE("Pre-filled letters at a crossing must be mutually consistent") {
  // 2x2 grid, top-left seeded 'A': the across slot (row 0) must start
  // with A, and the down slot (col 0) must also start with A -- only
  // AT/AN (down AN, TO... ) combinations consistent with both survive.
  auto grid = xfill::Grid::FromSpec({"A.", ".."});
  auto dict = WriteAndLoadDict("test_prefill_crossing.dict",
                                {"AT;10", "NO;10", "AN;10", "TO;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve();
  REQUIRE(solution.has_value());
  REQUIRE(solution->assignment.at(0) == "AT");
  REQUIRE(solution->assignment.at(2) == "AN");
}

TEST_CASE("SolveParallel with 1 thread finds the same solution as Solve()") {
  // num_threads=1 should be worker 0 alone, which is seeded to reproduce
  // today's single-threaded sequence exactly (attempt_offset=0).
  auto grid = xfill::Grid::FromSpec({"..", ".."});
  auto dict = WriteAndLoadDict("test_parallel_single.dict",
                                {"AT;10", "NO;10", "AN;10", "TO;10"});

  auto result = xfill::Solver::SolveParallel(grid, dict, /*num_threads=*/1);
  REQUIRE(result.solution.has_value());
  REQUIRE(result.num_threads == 1);
  REQUIRE(result.solution->assignment.at(0) == "AT");
  REQUIRE(result.solution->assignment.at(1) == "NO");
  REQUIRE(result.solution->assignment.at(2) == "AN");
  REQUIRE(result.solution->assignment.at(3) == "TO");
}

TEST_CASE("SolveParallel with several threads finds a valid solution") {
  auto grid = xfill::Grid::FromSpec({"...", "###", "..."});
  auto dict = WriteAndLoadDict("test_parallel_multi.dict",
                                {"CAT;50", "DOG;40"});

  auto result = xfill::Solver::SolveParallel(grid, dict, /*num_threads=*/4);
  REQUIRE(result.solution.has_value());
  REQUIRE(result.num_threads == 4);
  // Two non-crossing 3-letter slots, two 3-letter words -- either
  // assignment (CAT/DOG or DOG/CAT) is valid; just check it's a genuine,
  // non-duplicate use of the dictionary.
  const std::string& first = result.solution->assignment.at(0);
  const std::string& second = result.solution->assignment.at(1);
  REQUIRE(first != second);
  REQUIRE((first == "CAT" || first == "DOG"));
  REQUIRE((second == "CAT" || second == "DOG"));
}

TEST_CASE("SolveParallel proves no solution when every worker independently exhausts") {
  auto grid = xfill::Grid::FromSpec({"...."});
  auto dict = WriteAndLoadDict("test_parallel_unsat.dict", {"CAT;10"});

  auto result = xfill::Solver::SolveParallel(grid, dict, /*num_threads=*/4);
  REQUIRE_FALSE(result.solution.has_value());
  REQUIRE(result.num_threads == 4);
}

TEST_CASE("SolveParallel with num_threads=0 uses hardware_concurrency") {
  auto grid = xfill::Grid::FromSpec({"..."});
  auto dict = WriteAndLoadDict("test_parallel_auto.dict", {"CAT;10"});

  auto result = xfill::Solver::SolveParallel(grid, dict, /*num_threads=*/0);
  REQUIRE(result.solution.has_value());
  REQUIRE(result.num_threads >= 1);
}

TEST_CASE("Solve() with unlimited_budget still finds a solution") {
  auto grid = xfill::Grid::FromSpec({"..", ".."});
  auto dict = WriteAndLoadDict("test_unlimited_sat.dict",
                                {"AT;10", "NO;10", "AN;10", "TO;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve(0, nullptr, /*unlimited_budget=*/true);
  REQUIRE(solution.has_value());
  REQUIRE(solution->assignment.at(0) == "AT");
  REQUIRE(solution->assignment.at(1) == "NO");
  REQUIRE(solution->assignment.at(2) == "AN");
  REQUIRE(solution->assignment.at(3) == "TO");
}

TEST_CASE("Solve() with unlimited_budget still proves no solution, no restarts needed") {
  auto grid = xfill::Grid::FromSpec({"...."});
  auto dict = WriteAndLoadDict("test_unlimited_unsat.dict", {"CAT;10"});

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve(0, nullptr, /*unlimited_budget=*/true);
  REQUIRE_FALSE(solution.has_value());
  REQUIRE(solver.stats().restarts == 0);
}

TEST_CASE("Backtrack's large-domain word-shuffle branch still finds a valid solution") {
  // Two non-crossing 3-letter slots, but with more filler words of that
  // length than kCandidateDirectThreshold (1000) -- see solver.cpp's
  // Backtrack -- so branching exercises the large-domain candidate path
  // (a shuffled std::vector<size_t> extracted from dict_.ScoreOrder) rather
  // than the small-domain direct-lookup path this project's other tests
  // all stay under. attempt_offset=1 makes global_attempt=1 on Solve()'s
  // very first (only) attempt, so randomize_slot_choice_ is true and
  // attempt_offset_ > 0 skips kWordShuffleRestartThreshold's restart-count
  // gate (see solver.hpp's attempt_offset_ comment) -- the shuffled branch
  // runs from the start, not just after this solver instance has failed
  // its way through 20 restarts.
  auto grid = xfill::Grid::FromSpec({"...", "###", "..."});
  std::vector<std::string> words;
  words.reserve(1010);
  for (int i = 0; i < 1010; ++i) {
    std::string w;
    w += static_cast<char>('A' + (i / 26 / 26) % 26);
    w += static_cast<char>('A' + (i / 26) % 26);
    w += static_cast<char>('A' + i % 26);
    words.push_back(w + ";10");
  }
  auto dict = WriteAndLoadDict("test_large_domain_shuffle.dict", words);

  xfill::Solver solver(grid, dict);
  auto solution = solver.Solve(/*attempt_offset=*/1);
  REQUIRE(solution.has_value());
  // No crossings to satisfy here (row 1 is blocked) -- correctness just
  // means the no-duplicate-words rule still held under the shuffled path.
  REQUIRE(solution->assignment.at(0) != solution->assignment.at(1));
}

TEST_CASE("MaximizeScoreParallel's on_improved callbacks are monotonically increasing and match the returned solution") {
  // Regression test for a cross-thread reporting race: MaximizeBacktrack's
  // shared_best_score CAS ratchet correctly enforces a strictly-increasing
  // *score* sequence, but (before the fix this guards) the on_improved
  // callback invocations themselves were serialized only by callback_mutex,
  // whose acquisition order isn't tied to CAS order -- a worker that won an
  // earlier, lower CAS could still be scheduled to acquire the mutex AFTER
  // a worker that won a later, higher CAS, and call on_improved with its
  // own stale, lower score, overwriting a caller's "best solution so far"
  // with a worse one. Exercised with several threads and many repetitions
  // (a race is inherently timing-dependent, not something one run reliably
  // triggers) against this exact 2x2 fixture, already hand-verified
  // elsewhere this session to have exactly two complete solutions scoring
  // 102 and 140 -- enough distinct scores for a real ratchet sequence to
  // occur, not just a single trivial "first and only improvement".
  auto grid = xfill::Grid::FromSpec({"..", ".."});
  auto dict = WriteAndLoadDict(
      "test_maximize_race.dict",
      {"AB;90", "CD;10", "AC;1", "BD;1", "CA;20", "DB;20"});

  // Sums a solution's true total score by looking up each assigned word's
  // index (by text -- Dictionary has no word-index reverse lookup, and this
  // fixture is tiny enough that a linear scan per slot is fine for a test)
  // and reading WordScore for it -- independent of whatever score value the
  // solve's own bookkeeping reported, so this actually checks the returned
  // Solution is self-consistent with the dictionary, not just internally
  // consistent with itself.
  auto true_score = [&](const xfill::Solution& sol) {
    int64_t total = 0;
    for (const xfill::Slot& s : grid.slots()) {
      const std::string& word = sol.assignment.at(s.id);
      const std::vector<std::string>& words_of_length = dict.WordsOfLength(s.length);
      for (size_t i = 0; i < words_of_length.size(); ++i) {
        if (words_of_length[i] == word) {
          total += dict.WordScore(s.length, i);
          break;
        }
      }
    }
    return total;
  };

  for (int iteration = 0; iteration < 200; ++iteration) {
    std::mutex log_mutex;
    std::vector<int64_t> reported_scores;
    auto on_improved = [&](const xfill::Solution&, int64_t score) {
      std::lock_guard<std::mutex> lock(log_mutex);
      reported_scores.push_back(score);
    };

    auto best = xfill::Solver::MaximizeScoreParallel(grid, dict, /*num_threads=*/8, on_improved);
    REQUIRE(best.has_value());

    // The core property the race could break: every reported score is
    // strictly greater than the one before it. A worker reporting its own
    // stale, superseded score after a better one was already reported
    // would show up here as a non-increasing (or decreasing) step.
    for (size_t i = 1; i < reported_scores.size(); ++i) {
      REQUIRE(reported_scores[i] > reported_scores[i - 1]);
    }
    // The final returned Solution's real, independently-recomputed score
    // must match the last (highest) value ever reported -- this is exactly
    // what a stale overwrite would violate: the *returned* solution being
    // worse than what was actually already reported as achieved.
    REQUIRE_FALSE(reported_scores.empty());
    REQUIRE(true_score(*best) == reported_scores.back());
    // For this fixture specifically: the true optimum is 140 (CD/AB
    // across, giving CA/DB down), so an exhaustive maximize search should
    // always land there, every iteration.
    REQUIRE(reported_scores.back() == 140);
  }
}
