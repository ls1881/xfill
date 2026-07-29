#include "xfill/dictionary.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace xfill {

WordBitset::WordBitset(size_t num_words, bool all_set)
    : words_((num_words + 63) / 64, 0), num_words_(num_words) {
  if (all_set) {
    std::fill(words_.begin(), words_.end(), ~uint64_t{0});
    size_t rem = num_words_ % 64;
    if (rem != 0 && !words_.empty()) {
      words_.back() &= (uint64_t{1} << rem) - 1;
    }
  }
}

void WordBitset::Set(size_t index) {
  words_[index / 64] |= (uint64_t{1} << (index % 64));
}

void WordBitset::Clear(size_t index) {
  words_[index / 64] &= ~(uint64_t{1} << (index % 64));
}

void WordBitset::ClearAll() {
  std::fill(words_.begin(), words_.end(), uint64_t{0});
}

bool WordBitset::Test(size_t index) const {
  return (words_[index / 64] >> (index % 64)) & 1;
}

size_t WordBitset::Count() const {
  size_t total = 0;
  for (uint64_t w : words_) {
    total += static_cast<size_t>(__builtin_popcountll(w));
  }
  return total;
}

bool WordBitset::Any() const {
  for (uint64_t w : words_) {
    if (w) return true;
  }
  return false;
}

std::vector<size_t> WordBitset::SetBits() const {
  std::vector<size_t> out;
  for (size_t i = 0; i < words_.size(); ++i) {
    uint64_t w = words_[i];
    while (w != 0) {
      int bit = __builtin_ctzll(w);
      out.push_back(i * 64 + static_cast<size_t>(bit));
      w &= w - 1;  // clear the lowest set bit
    }
  }
  return out;
}

size_t WordBitset::First() const {
  for (size_t i = 0; i < words_.size(); ++i) {
    if (words_[i] != 0) {
      return i * 64 + static_cast<size_t>(__builtin_ctzll(words_[i]));
    }
  }
  return num_words_;
}

WordBitset& WordBitset::operator&=(const WordBitset& other) {
  for (size_t i = 0; i < words_.size(); ++i) words_[i] &= other.words_[i];
  return *this;
}

WordBitset& WordBitset::operator|=(const WordBitset& other) {
  for (size_t i = 0; i < words_.size(); ++i) words_[i] |= other.words_[i];
  return *this;
}

void WordBitset::AndNot(const WordBitset& other) {
  for (size_t i = 0; i < words_.size(); ++i) words_[i] &= ~other.words_[i];
}

bool WordBitset::Intersects(const WordBitset& other) const {
  for (size_t i = 0; i < words_.size(); ++i) {
    if (words_[i] & other.words_[i]) return true;
  }
  return false;
}

bool WordBitset::IsSubsetOf(const WordBitset& other) const {
  for (size_t i = 0; i < words_.size(); ++i) {
    if (words_[i] & ~other.words_[i]) return false;
  }
  return true;
}

namespace {
std::string Trim(const std::string& s) {
  size_t start = 0, end = s.size();
  while (start < end &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  while (end > start &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(start, end - start);
}
}  // namespace

Dictionary Dictionary::LoadFromFile(const std::string& path, int min_score) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("could not open dictionary: " + path);

  Dictionary dict;
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty()) continue;

    size_t semi = line.find(';');
    std::string word =
        semi == std::string::npos ? line : line.substr(0, semi);
    int score = 0;
    if (semi != std::string::npos) {
      try {
        score = std::stoi(line.substr(semi + 1));
      } catch (...) {
        score = 0;
      }
    }
    if (score < min_score) continue;
    word = Trim(word);
    if (word.empty()) continue;
    for (char& c : word) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    int length = static_cast<int>(word.size());
    dict.words_by_length_[length].push_back(word);
    dict.scores_by_length_[length].push_back(score);
  }

  for (auto& [length, words] : dict.words_by_length_) {
    size_t n = words.size();
    std::vector<std::array<WordBitset, 26>> masks(
        static_cast<size_t>(length));
    for (int p = 0; p < length; ++p) {
      for (int c = 0; c < 26; ++c) {
        masks[static_cast<size_t>(p)][static_cast<size_t>(c)] =
            WordBitset(n, false);
      }
    }
    for (size_t i = 0; i < n; ++i) {
      const std::string& w = words[i];
      for (int p = 0; p < length; ++p) {
        int c = w[static_cast<size_t>(p)] - 'A';
        if (c >= 0 && c < 26) {
          masks[static_cast<size_t>(p)][static_cast<size_t>(c)].Set(i);
        }
      }
    }
    dict.letter_masks_[length] = std::move(masks);
  }

  for (auto& [length, scores] : dict.scores_by_length_) {
    std::vector<size_t> order(scores.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&scores](size_t a, size_t b) {
      return scores[a] > scores[b];
    });
    dict.score_order_by_length_[length] = std::move(order);
  }

  return dict;
}

bool Dictionary::HasLength(int length) const {
  return words_by_length_.count(length) > 0;
}

size_t Dictionary::NumWordsOfLength(int length) const {
  auto it = words_by_length_.find(length);
  return it != words_by_length_.end() ? it->second.size() : 0;
}

const std::vector<std::string>& Dictionary::WordsOfLength(int length) const {
  static const std::vector<std::string> empty;
  auto it = words_by_length_.find(length);
  return it != words_by_length_.end() ? it->second : empty;
}

const WordBitset& Dictionary::LetterMask(int length, int position,
                                          char ch) const {
  static const WordBitset empty(0, false);
  auto it = letter_masks_.find(length);
  if (it == letter_masks_.end()) return empty;
  if (position < 0 || position >= length) return empty;
  int idx = ch - 'A';
  if (idx < 0 || idx >= 26) return empty;
  return it->second[static_cast<size_t>(position)][static_cast<size_t>(idx)];
}

WordBitset Dictionary::FullDomain(int length) const {
  return WordBitset(NumWordsOfLength(length), true);
}

const std::vector<size_t>& Dictionary::ScoreOrder(int length) const {
  static const std::vector<size_t> empty;
  auto it = score_order_by_length_.find(length);
  return it != score_order_by_length_.end() ? it->second : empty;
}

}  // namespace xfill
