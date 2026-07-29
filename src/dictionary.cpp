#include "xfill/dictionary.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

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
