#include "xfill/dictionary.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace xfill {

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

  // First pass: collect words per length into an unordered_map, since the
  // max length (and thus how big to size the index-by-length vectors
  // below) isn't known up front. This map only exists during loading; the
  // solver's hot path never touches it.
  std::unordered_map<int, std::vector<std::string>> words_by_length;
  std::unordered_map<int, std::vector<int>> scores_by_length;
  int max_length = 0;

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
    max_length = std::max(max_length, length);
    words_by_length[length].push_back(word);
    scores_by_length[length].push_back(score);
  }

  Dictionary dict;
  size_t num_lengths = static_cast<size_t>(max_length) + 1;
  dict.words_by_length_.resize(num_lengths);
  dict.scores_by_length_.resize(num_lengths);
  dict.letter_masks_.resize(num_lengths);
  dict.score_order_by_length_.resize(num_lengths);

  for (auto& [length, words] : words_by_length) {
    dict.words_by_length_[static_cast<size_t>(length)] = std::move(words);
  }
  for (auto& [length, scores] : scores_by_length) {
    dict.scores_by_length_[static_cast<size_t>(length)] = std::move(scores);
  }

  for (int length = 1; length < static_cast<int>(num_lengths); ++length) {
    const std::vector<std::string>& words =
        dict.words_by_length_[static_cast<size_t>(length)];
    size_t n = words.size();
    if (n == 0) continue;

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
    dict.letter_masks_[static_cast<size_t>(length)] = std::move(masks);
  }

  for (int length = 1; length < static_cast<int>(num_lengths); ++length) {
    const std::vector<int>& scores =
        dict.scores_by_length_[static_cast<size_t>(length)];
    std::vector<size_t> order(scores.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&scores](size_t a, size_t b) {
      return scores[a] > scores[b];
    });
    dict.score_order_by_length_[static_cast<size_t>(length)] = std::move(order);
  }

  return dict;
}

bool Dictionary::HasLength(int length) const {
  return length >= 0 && static_cast<size_t>(length) < words_by_length_.size() &&
         !words_by_length_[static_cast<size_t>(length)].empty();
}

size_t Dictionary::NumWordsOfLength(int length) const {
  if (length < 0 || static_cast<size_t>(length) >= words_by_length_.size()) {
    return 0;
  }
  return words_by_length_[static_cast<size_t>(length)].size();
}

const std::vector<std::string>& Dictionary::WordsOfLength(int length) const {
  static const std::vector<std::string> empty;
  if (length < 0 || static_cast<size_t>(length) >= words_by_length_.size()) {
    return empty;
  }
  return words_by_length_[static_cast<size_t>(length)];
}

const WordBitset& Dictionary::LetterMask(int length, int position,
                                          char ch) const {
  static const WordBitset empty(0, false);
  if (length < 0 || static_cast<size_t>(length) >= letter_masks_.size()) {
    return empty;
  }
  if (position < 0 || position >= length) return empty;
  int idx = ch - 'A';
  if (idx < 0 || idx >= 26) return empty;
  const std::vector<std::array<WordBitset, 26>>& masks =
      letter_masks_[static_cast<size_t>(length)];
  if (masks.empty()) return empty;
  return masks[static_cast<size_t>(position)][static_cast<size_t>(idx)];
}

WordBitset Dictionary::FullDomain(int length) const {
  return WordBitset(NumWordsOfLength(length), true);
}

const std::vector<size_t>& Dictionary::ScoreOrder(int length) const {
  static const std::vector<size_t> empty;
  if (length < 0 ||
      static_cast<size_t>(length) >= score_order_by_length_.size()) {
    return empty;
  }
  return score_order_by_length_[static_cast<size_t>(length)];
}

}  // namespace xfill
