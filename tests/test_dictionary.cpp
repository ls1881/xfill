#include <catch2/catch_test_macros.hpp>

#include "xfill/dictionary.hpp"

TEST_CASE("WordBitset basic set/clear/count") {
  xfill::WordBitset bs(10, /*all_set=*/true);
  REQUIRE(bs.Count() == 10);
  bs.Clear(3);
  REQUIRE(bs.Count() == 9);
  REQUIRE_FALSE(bs.Test(3));
  REQUIRE(bs.Test(4));
}

// TODO: Dictionary::LoadFromFile + LetterMask correctness tests once
// implemented (use data/wordlist_sample.txt as fixture).
