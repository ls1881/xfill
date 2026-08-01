#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <vector>

#include "xfill/dictionary.hpp"
#include "xfill/grid.hpp"
#include "xfill/solver.hpp"

namespace {

void WriteFilledGrid(std::ostream& out, const xfill::Grid& grid,
                      const xfill::Solution& solution) {
  int width = grid.width();
  int height = grid.height();
  std::vector<char> chars(static_cast<size_t>(width) * static_cast<size_t>(height), '#');

  for (int r = 0; r < height; ++r) {
    for (int c = 0; c < width; ++c) {
      if (!grid.IsBlocked(r, c)) {
        chars[static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c)] = '.';
      }
    }
  }

  for (const auto& slot : grid.slots()) {
    auto it = solution.assignment.find(slot.id);
    if (it == solution.assignment.end()) continue;
    const std::string& word = it->second;
    for (size_t k = 0; k < slot.cells.size(); ++k) {
      chars[static_cast<size_t>(slot.cells[k])] = word[k];
    }
  }

  for (int r = 0; r < height; ++r) {
    for (int c = 0; c < width; ++c) {
      out << chars[static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c)];
    }
    out << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: xfill_cli <grid_spec_file> <dictionary_file> "
                 "[min_score] [num_threads]\n"
                 "  num_threads: 0 (default) = "
                 "std::thread::hardware_concurrency(); 1 = single-threaded,\n"
                 "  for reproducible timing or comparing against a build "
                 "predating SolveParallel.\n";
    return 1;
  }

  try {
    int min_score = argc >= 4 ? std::stoi(argv[3]) : 0;
    unsigned num_threads = argc >= 5 ? static_cast<unsigned>(std::stoul(argv[4])) : 0;

    xfill::Grid grid = xfill::Grid::FromFile(argv[1]);
    xfill::Dictionary dict =
        xfill::Dictionary::LoadFromFile(argv[2], min_score);

    auto start = std::chrono::steady_clock::now();
    xfill::ParallelSolveResult result =
        xfill::Solver::SolveParallel(grid, dict, num_threads);
    auto end = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();

    std::filesystem::path output_dir = "output";
    std::filesystem::create_directories(output_dir);
    std::filesystem::path output_path =
        output_dir / (std::filesystem::path(argv[1]).stem().string() + "_output.txt");
    std::ofstream out(output_path, std::ios::trunc);

    // Same output to both: stdout for interactive use and for
    // benchmarks/bench_subset.py, which parses the stats line from it;
    // the file for inspecting a specific grid's fill later, since a
    // terminal scrolls away but the file persists across runs.
    for (std::ostream* stream : {static_cast<std::ostream*>(&std::cout),
                                  static_cast<std::ostream*>(&out)}) {
      if (!result.solution) {
        *stream << "No solution found.\n";
      } else {
        WriteFilledGrid(*stream, grid, *result.solution);
      }
      // "restarts=... time=...s" must stay contiguous, in this order:
      // benchmarks/bench_subset.py's STATS_RE depends on that exact
      // substring, so num_threads is appended after time, not between.
      *stream << "\nnodes=" << result.stats.nodes
              << " backtracks=" << result.stats.backtracks
              << " restarts=" << result.stats.restarts
              << " time=" << seconds << "s"
              << " threads=" << result.num_threads << "\n";
    }

    std::cerr << "wrote " << output_path.string() << "\n";
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
