#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"
#include "xfill/dictionary.hpp"

using xfill_test::WriteAndLoadDict;

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

TEST_CASE("Dictionary rejects entries that aren't pure A-Z after uppercasing") {
  // Regression test: the real wordlist contains a handful of entries like
  // "ENTREE3000" and "ARTHURC4CLARKE" -- letters mixed with digits. Every
  // downstream consumer (LetterMask's construction, and Solver::Propagate's
  // direct-lookup path, which reads a candidate word's raw characters and
  // shifts by `ch - 'A'`) assumes every loaded word is pure A-Z; letting a
  // non-letter character through was confirmed via UBSan to cause
  // undefined behavior (a negative/oversized shift). Such entries must be
  // rejected at load time instead, the same as an empty word already is.
  auto dict = WriteAndLoadDict("test_dict_nonalpha.dict",
                                {"CAT;50", "CA7;50", "DO9;50", "CATS;30"});
  REQUIRE(dict.NumWordsOfLength(3) == 1);  // only CAT -- CA7 rejected
  REQUIRE(dict.NumWordsOfLength(4) == 1);  // CATS; DO9 would be length 3
}
