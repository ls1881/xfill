#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xfill {

// A fixed-size bitset over word indices for a given word length, sized to
// fit the dictionary's word count for that length. Backed by a flat
// vector<uint64_t> so operations compile to wide vector instructions.
class WordBitset {
 public:
  explicit WordBitset(size_t num_words, bool all_set = true);

  void Clear(size_t index);
  bool Test(size_t index) const;
  size_t Count() const;

  WordBitset& operator&=(const WordBitset& other);

 private:
  std::vector<uint64_t> words_;
  size_t num_words_;
};

class Dictionary {
 public:
  // Load one word per line. Words are grouped internally by length.
  static Dictionary LoadFromFile(const std::string& path);

  const std::vector<std::string>& WordsOfLength(int length) const;

  // Bitset of all words of `length` whose character at `position` is `ch`.
  // Precomputed at load time for O(1) lookup during propagation.
  const WordBitset& LetterMask(int length, int position, char ch) const;

 private:
  std::unordered_map<int, std::vector<std::string>> words_by_length_;
  // Keyed by (length, position, letter) -> precomputed mask.
  // TODO: pick a concrete map/array layout once slot-length range is known.
};

}  // namespace xfill
