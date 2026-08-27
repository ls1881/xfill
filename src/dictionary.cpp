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

struct ParsedEntry {
  std::string word;
  int score;
};

// Parses a "WORD;SCORE" file (semicolon-delimited; a missing/unparseable
// score defaults to 0) into per-length lists, dropping entries below
// `min_score`'s threshold *for that entry's own length* and any entry
// that isn't pure A-Z after uppercasing (see the UBSan note below). The
// score check has to happen after the word is normalized (trimmed,
// uppercased) and its length is known -- min_score.For() needs that
// length, whereas the old single-int threshold didn't care. Shared by
// LoadFromFile and LoadDual.
std::unordered_map<int, std::vector<ParsedEntry>> ParseWordFile(
    const std::string& path, const MinScoreByLength& min_score) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("could not open dictionary: " + path);

  std::unordered_map<int, std::vector<ParsedEntry>> by_length;
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
    word = Trim(word);
    if (word.empty()) continue;
    for (char& c : word) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    // A crossword cell only ever holds one A-Z letter, and every reader
    // of these words (LetterMask's construction, Propagate's direct-
    // lookup path) assumes that's true of everything loaded -- so an
    // entry that isn't pure A-Z after uppercasing (the real wordlist has
    // a few, e.g. "ENTREE3000") is rejected here, at the boundary, rather
    // than mishandled downstream: letting one through was confirmed via
    // UBSan to cause undefined behavior in Propagate's `1u << (ch - 'A')`.
    bool all_letters = std::all_of(word.begin(), word.end(),
                                    [](char c) { return c >= 'A' && c <= 'Z'; });
    if (!all_letters) continue;

    int length = static_cast<int>(word.size());
    if (score < min_score.For(length)) continue;
    by_length[length].push_back({std::move(word), score});
  }
  return by_length;
}
}  // namespace

void Dictionary::BuildDerivedIndexes() {
  size_t num_lengths = words_by_length_.size();
  letter_masks_.resize(num_lengths);
  score_order_by_length_.resize(num_lengths);
  score_rank_by_length_.resize(num_lengths);

  for (int length = 1; length < static_cast<int>(num_lengths); ++length) {
    const std::vector<std::string>& words = words_by_length_[static_cast<size_t>(length)];
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
    letter_masks_[static_cast<size_t>(length)] = std::move(masks);
  }

  for (int length = 1; length < static_cast<int>(num_lengths); ++length) {
    const std::vector<int>& scores = scores_by_length_[static_cast<size_t>(length)];
    std::vector<size_t> order(scores.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&scores](size_t a, size_t b) {
      return scores[a] > scores[b];
    });
    std::vector<size_t> rank(order.size());
    for (size_t pos = 0; pos < order.size(); ++pos) rank[order[pos]] = pos;
    score_rank_by_length_[static_cast<size_t>(length)] = std::move(rank);
    score_order_by_length_[static_cast<size_t>(length)] = std::move(order);
  }
}

Dictionary Dictionary::LoadFromFile(const std::string& path, MinScoreByLength min_score) {
  std::unordered_map<int, std::vector<ParsedEntry>> parsed = ParseWordFile(path, min_score);

  int max_length = 0;
  for (const auto& [length, entries] : parsed) max_length = std::max(max_length, length);

  Dictionary dict;
  size_t num_lengths = static_cast<size_t>(max_length) + 1;
  dict.words_by_length_.resize(num_lengths);
  dict.scores_by_length_.resize(num_lengths);
  dict.allowed_across_by_length_.resize(num_lengths);
  dict.allowed_down_by_length_.resize(num_lengths);

  for (auto& [length, entries] : parsed) {
    std::vector<std::string>& words = dict.words_by_length_[static_cast<size_t>(length)];
    std::vector<int>& scores = dict.scores_by_length_[static_cast<size_t>(length)];
    words.reserve(entries.size());
    scores.reserve(entries.size());
    for (auto& e : entries) {
      words.push_back(std::move(e.word));
      scores.push_back(e.score);
    }
    // No per-direction restriction -- every word this Dictionary knows is
    // usable in either direction (see AllowedMask's doc comment).
    dict.allowed_across_by_length_[static_cast<size_t>(length)] =
        WordBitset(words.size(), /*all_set=*/true);
    dict.allowed_down_by_length_[static_cast<size_t>(length)] =
        WordBitset(words.size(), /*all_set=*/true);
  }

  dict.BuildDerivedIndexes();
  return dict;
}

Dictionary Dictionary::LoadDual(const std::string& across_path, MinScoreByLength min_score_across,
                                 const std::string& down_path, MinScoreByLength min_score_down) {
  std::unordered_map<int, std::vector<ParsedEntry>> across =
      ParseWordFile(across_path, min_score_across);
  std::unordered_map<int, std::vector<ParsedEntry>> down =
      ParseWordFile(down_path, min_score_down);

  int max_length = 0;
  for (const auto& [length, entries] : across) max_length = std::max(max_length, length);
  for (const auto& [length, entries] : down) max_length = std::max(max_length, length);

  Dictionary dict;
  size_t num_lengths = static_cast<size_t>(max_length) + 1;
  dict.words_by_length_.resize(num_lengths);
  dict.scores_by_length_.resize(num_lengths);
  dict.allowed_across_by_length_.resize(num_lengths);
  dict.allowed_down_by_length_.resize(num_lengths);

  struct Merged {
    int score = 0;
    bool in_across = false;
    bool in_down = false;
  };

  for (int length = 1; length < static_cast<int>(num_lengths); ++length) {
    std::unordered_map<std::string, Merged> merged;
    auto add = [&merged](const std::vector<ParsedEntry>& entries, bool is_across) {
      for (const ParsedEntry& e : entries) {
        Merged& m = merged[e.word];
        m.score = std::max(m.score, e.score);
        (is_across ? m.in_across : m.in_down) = true;
      }
    };
    auto it_a = across.find(length);
    if (it_a != across.end()) add(it_a->second, /*is_across=*/true);
    auto it_d = down.find(length);
    if (it_d != down.end()) add(it_d->second, /*is_across=*/false);
    if (merged.empty()) continue;

    // unordered_map iteration order isn't reproducible run to run; sort so
    // a given pair of input files always yields the same word indices.
    std::vector<std::string> words;
    words.reserve(merged.size());
    for (auto& [w, m] : merged) words.push_back(w);
    std::sort(words.begin(), words.end());

    std::vector<int>& scores = dict.scores_by_length_[static_cast<size_t>(length)];
    scores.reserve(words.size());
    WordBitset allowed_across(words.size(), /*all_set=*/false);
    WordBitset allowed_down(words.size(), /*all_set=*/false);
    for (size_t i = 0; i < words.size(); ++i) {
      const Merged& m = merged[words[i]];
      scores.push_back(m.score);
      if (m.in_across) allowed_across.Set(i);
      if (m.in_down) allowed_down.Set(i);
    }
    dict.words_by_length_[static_cast<size_t>(length)] = std::move(words);
    dict.allowed_across_by_length_[static_cast<size_t>(length)] = std::move(allowed_across);
    dict.allowed_down_by_length_[static_cast<size_t>(length)] = std::move(allowed_down);
  }

  dict.BuildDerivedIndexes();
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
