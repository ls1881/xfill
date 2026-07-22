#include "xfill/dictionary.hpp"

#include <fstream>
#include <stdexcept>

namespace xfill {

WordBitset::WordBitset(size_t num_words, bool all_set)
    : words_((num_words + 63) / 64, all_set ? ~uint64_t{0} : uint64_t{0}),
      num_words_(num_words) {
  // TODO: mask off trailing bits beyond num_words_ in the last word.
}

void WordBitset::Clear(size_t index) {
  words_[index / 64] &= ~(uint64_t{1} << (index % 64));
}

bool WordBitset::Test(size_t index) const {
  return (words_[index / 64] >> (index % 64)) & 1;
}

size_t WordBitset::Count() const {
  size_t total = 0;
  for (uint64_t w : words_) total += __builtin_popcountll(w);
  return total;
}

WordBitset& WordBitset::operator&=(const WordBitset& other) {
  for (size_t i = 0; i < words_.size(); ++i) words_[i] &= other.words_[i];
  return *this;
}

Dictionary Dictionary::LoadFromFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("could not open dictionary: " + path);

  Dictionary dict;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    dict.words_by_length_[static_cast<int>(line.size())].push_back(line);
  }

  // TODO: build LetterMask precomputed tables here.
  return dict;
}

const std::vector<std::string>& Dictionary::WordsOfLength(int length) const {
  static const std::vector<std::string> empty;
  auto it = words_by_length_.find(length);
  return it != words_by_length_.end() ? it->second : empty;
}

const WordBitset& Dictionary::LetterMask(int length, int position,
                                          char ch) const {
  // TODO: implement once the precomputed table exists.
  throw std::logic_error("LetterMask not yet implemented");
}

}  // namespace xfill
