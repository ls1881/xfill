#include <chrono>
#include <iostream>
#include <vector>

#include "xfill/dictionary.hpp"
#include "xfill/grid.hpp"
#include "xfill/solver.hpp"

namespace {

void PrintFilledGrid(const xfill::Grid& grid, const xfill::Solution& solution) {
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
      std::cout << chars[static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c)];
    }
    std::cout << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: xfill_cli <grid_spec_file> <dictionary_file>\n";
    return 1;
  }

  try {
    xfill::Grid grid = xfill::Grid::FromFile(argv[1]);
    xfill::Dictionary dict = xfill::Dictionary::LoadFromFile(argv[2]);

    xfill::Solver solver(grid, dict);

    auto start = std::chrono::steady_clock::now();
    auto solution = solver.Solve();
    auto end = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();

    if (!solution) {
      std::cout << "No solution found.\n";
    } else {
      PrintFilledGrid(grid, *solution);
    }

    std::cerr << "\nnodes=" << solver.stats().nodes
               << " backtracks=" << solver.stats().backtracks
               << " time=" << seconds << "s\n";
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
