#pragma once

#include <array>
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
  WordBitset() : num_words_(0) {}
  explicit WordBitset(size_t num_words, bool all_set = true);

  void Set(size_t index);
  void Clear(size_t index);
  bool Test(size_t index) const;
  size_t Count() const;
  bool Any() const;
  size_t size() const { return num_words_; }

  // Indices of every set bit. Simple O(n) scan -- correctness-first;
  // revisit only if profiling on real grids says it's worth it.
  std::vector<size_t> SetBits() const;

  WordBitset& operator&=(const WordBitset& other);
  WordBitset& operator|=(const WordBitset& other);

 private:
  std::vector<uint64_t> words_;
  size_t num_words_;
};

class Dictionary {
 public:
  Dictionary() = default;

  // Loads a "WORD;SCORE" file, one entry per line (semicolon-delimited;
  // a missing/unparseable score defaults to 0). Words are grouped
  // internally by length. Score is retained but not yet used by the
  // solver -- a natural hook for quality-guided search later.
  static Dictionary LoadFromFile(const std::string& path);

  bool HasLength(int length) const;
  size_t NumWordsOfLength(int length) const;
  const std::vector<std::string>& WordsOfLength(int length) const;

  // Bitset of all words of `length` whose character at `position` is `ch`.
  // Precomputed at load time for O(1) lookup during propagation.
  const WordBitset& LetterMask(int length, int position, char ch) const;

  // A domain bitset with every word of `length` set -- i.e. "no
  // constraints applied yet". Empty (all-zero, zero-length) if the
  // dictionary has no words of that length.
  WordBitset FullDomain(int length) const;

 private:
  std::unordered_map<int, std::vector<std::string>> words_by_length_;
  std::unordered_map<int, std::vector<int>> scores_by_length_;
  // letter_masks_[length][position][letter - 'A']
  std::unordered_map<int, std::vector<std::array<WordBitset, 26>>>
      letter_masks_;
};

}  // namespace xfill
