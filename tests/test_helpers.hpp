#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "xfill/dictionary.hpp"

namespace xfill_test {

// Writes `lines` to a temporary file, loads it as a Dictionary, then
// deletes the file -- lets a test express its wordlist inline instead of
// checking in a fixture file. Shared by test_dictionary.cpp and
// test_solver.cpp.
inline xfill::Dictionary WriteAndLoadDict(const std::string& path,
                                           const std::vector<std::string>& lines) {
  std::ofstream out(path);
  for (const auto& line : lines) out << line << "\n";
  out.close();
  auto dict = xfill::Dictionary::LoadFromFile(path);
  std::remove(path.c_str());
  return dict;
}

}  // namespace xfill_test
