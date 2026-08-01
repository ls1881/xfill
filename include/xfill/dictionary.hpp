#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace xfill {

// A fixed-size bitset over word indices for a given word length, sized to
// fit the dictionary's word count for that length. Backed by a flat
// vector<uint64_t> so operations compile to wide vector instructions.
//
// Every method is defined right here in the header (not in dictionary.cpp,
// where they used to live) so they can actually be inlined at their call
// sites in solver.cpp's hot propagation loop: this project builds without
// LTO/IPO (see CMakeLists.txt), so a definition left in a different
// translation unit is a real, uninlinable function call at -O3 regardless
// of how trivial its body is. `sample`-profiling a real timing-out 15x15
// (grid_013.txt) showed this directly -- operator|= alone was the single
// hottest symbol in the whole binary (27% of samples), ahead of Propagate
// itself, purely from cross-TU call overhead on a one-line loop. See
// docs/design.md for the measured effect of moving these here.
class WordBitset {
 public:
  WordBitset() : num_words_(0) {}
  explicit WordBitset(size_t num_words, bool all_set = true)
      : words_((num_words + 63) / 64, 0), num_words_(num_words) {
    if (all_set) {
      std::fill(words_.begin(), words_.end(), ~uint64_t{0});
      size_t rem = num_words_ % 64;
      if (rem != 0 && !words_.empty()) {
        words_.back() &= (uint64_t{1} << rem) - 1;
      }
    }
  }

  void Set(size_t index) { words_[index / 64] |= (uint64_t{1} << (index % 64)); }
  void Clear(size_t index) { words_[index / 64] &= ~(uint64_t{1} << (index % 64)); }
  // Zeros every word without changing size() or reallocating -- lets a
  // caller reuse one WordBitset as scratch space across many iterations
  // of a hot loop instead of constructing (and heap-allocating) a fresh
  // one each time.
  void ClearAll() { std::fill(words_.begin(), words_.end(), uint64_t{0}); }
  bool Test(size_t index) const { return (words_[index / 64] >> (index % 64)) & 1; }
  size_t Count() const {
    size_t total = 0;
    for (uint64_t w : words_) total += static_cast<size_t>(__builtin_popcountll(w));
    return total;
  }

  // Count of bits set here but not in `other` -- i.e. |*this & ~other| --
  // without materializing the intersection. Lets a caller that only wants
  // a count (e.g. SelectBranchSlot scoring a slot against the "used words"
  // mask) skip copying this bitset just to AndNot() it and Count() the
  // result.
  size_t CountAndNot(const WordBitset& other) const {
    size_t total = 0;
    for (size_t i = 0; i < words_.size(); ++i) {
      total += static_cast<size_t>(__builtin_popcountll(words_[i] & ~other.words_[i]));
    }
    return total;
  }
  bool Any() const {
    for (uint64_t w : words_) {
      if (w) return true;
    }
    return false;
  }
  size_t size() const { return num_words_; }

  // Indices of every set bit, via ctz + clear-lowest-bit so cost tracks
  // the number of *chunks touched and bits actually set*, not size().
  // `reserve_hint`, when the caller already knows the exact (or an upper
  // bound on the) popcount -- e.g. Propagate, which just computed this via
  // Count() to pick the smallest queued domain -- lets the result vector
  // be allocated once instead of growing via push_back's amortized
  // doubling, which was a real cost in that hot path.
  std::vector<size_t> SetBits(size_t reserve_hint = 0) const {
    std::vector<size_t> out;
    out.reserve(reserve_hint);
    AppendSetBits(out);
    return out;
  }

  // Same traversal as SetBits(), but appends into a caller-supplied vector
  // instead of returning a fresh one -- lets a hot-path caller (e.g.
  // Propagate, which does this once per popped queue slot) reuse one
  // scratch vector across many calls instead of allocating/freeing a new
  // one every time. Caller is responsible for clearing `out` first if a
  // clean result (rather than an appended one) is wanted.
  //
  // `max_bits`, when the caller already knows exactly how many bits are
  // set (e.g. Propagate, which just read this domain's cached popcount),
  // stops the scan the moment that many bits have been found instead of
  // continuing to check every remaining chunk for zero -- for a narrow
  // domain (a singleton is the extreme case) whose one surviving word
  // happens to sit in a large dictionary's bitset, that tail of trailing
  // all-zero chunks can otherwise dwarf the actual work.
  void AppendSetBits(std::vector<size_t>& out,
                      size_t max_bits = std::numeric_limits<size_t>::max()) const {
    size_t found = 0;
    for (size_t i = 0; i < words_.size() && found < max_bits; ++i) {
      uint64_t w = words_[i];
      while (w != 0) {
        int bit = __builtin_ctzll(w);
        out.push_back(i * 64 + static_cast<size_t>(bit));
        w &= w - 1;  // clear the lowest set bit
        ++found;
      }
    }
  }

  // Index of the first (lowest) set bit. Caller must ensure Any() is true.
  size_t First() const {
    for (size_t i = 0; i < words_.size(); ++i) {
      if (words_[i] != 0) {
        return i * 64 + static_cast<size_t>(__builtin_ctzll(words_[i]));
      }
    }
    return num_words_;
  }

  WordBitset& operator&=(const WordBitset& other) {
    for (size_t i = 0; i < words_.size(); ++i) words_[i] &= other.words_[i];
    return *this;
  }

  // Intersects with `other` in place and returns the popcount of the
  // result, in one pass over the chunk array -- fuses what would
  // otherwise be an operator&=() pass followed by a separate Count()
  // (or Any()) pass over the same, now-narrowed data. A caller that
  // needs both the narrowed domain and its new size (e.g. Propagate,
  // narrowing a crossing neighbor and then needing its count to
  // re-queue it) gets both for the cost of one traversal instead of two.
  size_t AndAssignCount(const WordBitset& other) {
    size_t total = 0;
    for (size_t i = 0; i < words_.size(); ++i) {
      words_[i] &= other.words_[i];
      total += static_cast<size_t>(__builtin_popcountll(words_[i]));
    }
    return total;
  }
  WordBitset& operator|=(const WordBitset& other) {
    for (size_t i = 0; i < words_.size(); ++i) words_[i] |= other.words_[i];
    return *this;
  }

  // True if any bit is set in both -- cheaper than materializing (*this &
  // other).Any() since it can stop at the first shared word.
  bool Intersects(const WordBitset& other) const {
    for (size_t i = 0; i < words_.size(); ++i) {
      if (words_[i] & other.words_[i]) return true;
    }
    return false;
  }

  // True if every bit set here is also set in `other` -- i.e. intersecting
  // with `other` would remove nothing. Lets a caller skip a narrowing step
  // that wouldn't actually narrow anything.
  bool IsSubsetOf(const WordBitset& other) const {
    for (size_t i = 0; i < words_.size(); ++i) {
      if (words_[i] & ~other.words_[i]) return false;
    }
    return true;
  }

 private:
  std::vector<uint64_t> words_;
  size_t num_words_;
};

class Dictionary {
 public:
  Dictionary() = default;

  // Loads a "WORD;SCORE" file, one entry per line (semicolon-delimited;
  // a missing/unparseable score defaults to 0). Words are grouped
  // internally by length. Entries scoring below `min_score` are dropped
  // entirely -- not just deprioritized -- so the solver can never place them.
  static Dictionary LoadFromFile(const std::string& path, int min_score = 0);

  bool HasLength(int length) const;
  size_t NumWordsOfLength(int length) const;
  const std::vector<std::string>& WordsOfLength(int length) const;

  // Bitset of all words of `length` whose character at `position` is `ch`.
  // Precomputed at load time for O(1) lookup during propagation. Defined
  // right here (not in dictionary.cpp, where it used to live), same
  // reason as WordBitset's methods above: this project builds without
  // LTO/IPO, so a definition left in a different translation unit is a
  // real, uninlinable call regardless of triviality, and this one is
  // called from Propagate's hottest inner loop, up to 26 times per
  // crossing. An earlier attempt moved this along with five other
  // trivial Dictionary accessors at once and regressed; moved alone this
  // time, it's a real ~1.7% win -- see docs/design.md for both results
  // and why only this one function was retested.
  const WordBitset& LetterMask(int length, int position, char ch) const {
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

  // A domain bitset with every word of `length` set -- i.e. "no
  // constraints applied yet". Empty (all-zero, zero-length) if the
  // dictionary has no words of that length.
  WordBitset FullDomain(int length) const;

  // Word indices of `length`, ordered by descending score (ties broken by
  // original index). Lets the solver try higher-quality words first without
  // re-sorting a domain's candidates on every branch. Empty if the
  // dictionary has no words of that length.
  const std::vector<size_t>& ScoreOrder(int length) const;

 private:
  // Indexed directly by length (index 0 unused) rather than keyed in an
  // unordered_map: word lengths are a small, dense range known at load
  // time, so a hash + bucket lookup on every access (LetterMask is called
  // from Propagate's hot inner loop, up to 26 times per crossing) is pure
  // overhead compared to a direct vector index.
  std::vector<std::vector<std::string>> words_by_length_;
  std::vector<std::vector<int>> scores_by_length_;
  // letter_masks_[length][position][letter - 'A']
  std::vector<std::vector<std::array<WordBitset, 26>>> letter_masks_;
  std::vector<std::vector<size_t>> score_order_by_length_;
};

}  // namespace xfill
