#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>

#include "xfill/dictionary.hpp"

TEST_CASE("WordBitset basic set/clear/count") {
  xfill::WordBitset bs(10, /*all_set=*/true);
  REQUIRE(bs.Count() == 10);
  bs.Clear(3);
  REQUIRE(bs.Count() == 9);
  REQUIRE_FALSE(bs.Test(3));
  REQUIRE(bs.Test(4));

  xfill::WordBitset empty(10, /*all_set=*/false);
  REQUIRE(empty.Count() == 0);
  REQUIRE_FALSE(empty.Any());
  empty.Set(7);
  REQUIRE(empty.Any());
  REQUIRE(empty.SetBits() == std::vector<size_t>{7});
}

TEST_CASE("WordBitset AND/OR combine as expected") {
  xfill::WordBitset a(4, false);
  a.Set(0);
  a.Set(1);
  xfill::WordBitset b(4, false);
  b.Set(1);
  b.Set(2);

  xfill::WordBitset and_result = a;
  and_result &= b;
  REQUIRE(and_result.SetBits() == std::vector<size_t>{1});

  xfill::WordBitset or_result = a;
  or_result |= b;
  REQUIRE(or_result.SetBits() == std::vector<size_t>{0, 1, 2});
}

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

TEST_CASE("Dictionary parses WORD;SCORE lines and groups by length") {
  auto dict = WriteAndLoadDict("test_dict_load.dict",
                                {"CAT;50", "DOG;40", "CATS;30"});
  REQUIRE(dict.NumWordsOfLength(3) == 2);
  REQUIRE(dict.NumWordsOfLength(4) == 1);
  REQUIRE_FALSE(dict.HasLength(5));
}

TEST_CASE("Dictionary::LetterMask identifies words with a letter at a position") {
  auto dict = WriteAndLoadDict("test_dict_mask.dict",
                                {"CAT;50", "COT;40", "DOG;40"});
  // Words of length 3 with 'A' at position 1: only CAT.
  const auto& mask = dict.LetterMask(3, 1, 'A');
  REQUIRE(mask.Count() == 1);
}
