#include <iostream>

#include "xfill/dictionary.hpp"
#include "xfill/grid.hpp"
#include "xfill/solver.hpp"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: xfill_cli <grid_spec_file> <dictionary_file>\n";
    return 1;
  }

  // TODO: read grid spec file into vector<string>, call Grid::FromSpec.
  // TODO: load dictionary, construct Solver, print result + stats.

  std::cout << "xfill: scaffold only, solver not yet implemented\n";
  return 0;
}
