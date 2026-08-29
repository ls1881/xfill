#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xfill {

enum class Direction { Across, Down };

// A single fillable run of cells (an "across" or "down" slot).
struct Slot {
  int id;
  Direction dir;
  int row;
  int col;
  // The slot's real spelled-out word length -- the sum of cell_lengths, NOT
  // necessarily cells.size(). Equal to cells.size() for an ordinary slot;
  // longer than it when the slot covers a rebus cell (see
  // Grid::RebusContent), since a rebus cell contributes more than one
  // character to the word it's part of. Every Dictionary/word-length
  // lookup uses this; only per-cell iteration should ever use cells.size()
  // or cell_lengths.
  int length;
  // Cell indices (row * width + col) covered by this slot, in order.
  std::vector<int> cells;
  // Parallel to cells: how many characters of the slot's word each cell
  // contributes -- 1 for an ordinary cell, or RebusContent(cell).size()
  // for a rebus cell. sum(cell_lengths) == length.
  std::vector<int> cell_lengths;
  // Parallel to cells: the word-character position (0-indexed into a
  // `length`-long word) each cell's content starts at -- the cumulative
  // sum of cell_lengths up to (not including) that cell. For an ordinary
  // slot (no rebus cells) this is just the identity, cell_char_start[k]
  // == k, matching cells.size() == length. Precomputed here (rather than
  // recomputed by each of BuildInitialDomains/ComputeCrossings/
  // FilledGridRows) since it's needed in all three places.
  std::vector<int> cell_char_start;
};

// Which slots cross at a given cell, and at what offset within each.
// For a crossing at an ordinary cell this is the cell's single word
// position in each slot, same as always. A crossing at a rebus cell is
// decomposed into several of these (one per character of the rebus
// content, see Grid::ComputeCrossings) rather than represented here as a
// single multi-character span -- so offset_a/offset_b are always exactly
// one word-character position, never a range. This decomposition is
// sound (a substring equality is logically equivalent to the AND of its
// per-character equalities, so it can never let an invalid final
// assignment through) but not maximally tight the way jointly reasoning
// about the whole substring would be -- Solver::Propagate may occasionally
// take a few extra nodes to notice a rebus-adjacent contradiction it
// could have caught immediately with joint reasoning. Correctness doesn't
// depend on that tightness (BuildInitialDomains's prefilled-letter
// narrowing plus ordinary backtracking are exact regardless), and this
// keeps Propagate's hot path completely untouched.
struct Crossing {
  int slot_a;
  int offset_a;
  int slot_b;
  int offset_b;
};

// A single rebus cell's real content, as supplied in a grid spec's
// optional trailing section (see Grid::FromFile) -- e.g. {0, 0, "AD"} for
// a rebus square at row 0, column 0 holding "AD".
struct RebusEntry {
  int row;
  int col;
  std::string content;
};

class Grid {
 public:
  Grid(int width, int height);

  // Loads a grid layout from a text spec: '.' = open cell, '#' = block,
  // A-Z (case-insensitive) = an open cell pre-filled with that letter --
  // a seeded/partial fill, e.g. a themed starting entry. A pre-filled
  // cell is otherwise an ordinary cell: it still belongs to whatever
  // across/down slot(s) cover it, just with that slot's domain
  // pre-narrowed to words matching the letter at that position (see
  // Solver::Solve).
  //
  // `rebus` optionally upgrades specific cells to hold more than one
  // character (see RebusContent): each entry's row/col must be in bounds,
  // not a block, pure A-Z after uppercasing, and its first character must
  // match `rows`' own character at that cell (the same "first letter"
  // stand-in convention .puz's main solution grid uses for a rebus
  // square) -- throws std::invalid_argument otherwise. Empty by default,
  // so every existing call site is unaffected.
  static Grid FromSpec(const std::vector<std::string>& rows,
                        const std::vector<RebusEntry>& rebus = {});

  // Reads a grid spec from a file: grid rows exactly as FromSpec expects,
  // one per line (trailing blank lines before any rebus section are
  // ignored; '\r' is stripped for files with CRLF endings). The FIRST
  // blank line found ends the grid-row section; every line after it (if
  // any) is an optional rebus entry "row,col:CONTENT", e.g. "0,0:AD" --
  // see RebusEntry. Absent for any grid with no rebus squares, so an
  // ordinary grid-spec file parses identically to before this existed.
  static Grid FromFile(const std::string& path);

  int width() const { return width_; }
  int height() const { return height_; }
  bool IsBlocked(int row, int col) const {
    return blocked_[static_cast<size_t>(row) * static_cast<size_t>(width_) +
                     static_cast<size_t>(col)];
  }
  // 'A'-'Z' if this cell is seeded with a pre-filled letter, '\0' if not
  // (including for a blocked cell, which is never pre-filled). For a
  // rebus cell, this is only the first character (see RebusContent for
  // the real, full content) -- the single-character stand-in used
  // wherever exactly one character is needed.
  char PrefilledLetter(int row, int col) const {
    return prefilled_[static_cast<size_t>(row) * static_cast<size_t>(width_) +
                       static_cast<size_t>(col)];
  }
  // Same, but by the flat cell index (row * width + col) a Slot::cells
  // entry already is -- avoids every caller re-deriving row/col from it.
  char PrefilledLetter(int cell) const { return prefilled_[static_cast<size_t>(cell)]; }

  // This cell's real rebus content (e.g. "AD"), or an empty string if
  // it's not a rebus cell -- including for a blank or blocked cell.
  const std::string& RebusContent(int row, int col) const {
    return rebus_content_[static_cast<size_t>(row) * static_cast<size_t>(width_) +
                           static_cast<size_t>(col)];
  }
  const std::string& RebusContent(int cell) const { return rebus_content_[static_cast<size_t>(cell)]; }

  const std::vector<Slot>& slots() const { return slots_; }
  const Slot& SlotById(int id) const {
    return slots_[static_cast<size_t>(id)];
  }
  const std::vector<Crossing>& crossings() const { return crossings_; }

 private:
  // Scans each row, then each column, for maximal runs of open cells;
  // any run of length >= 2 becomes a Slot (a length-1 run has nothing to
  // cross, so it can never be a real crossword entry). Also computes each
  // slot's cell_lengths/length from rebus_content_.
  void ComputeSlots();
  // Any cell covered by both an across and a down slot is a Crossing
  // between them. An ordinary cell contributes one Crossing at its single
  // shared word position; a rebus cell contributes one Crossing PER
  // CHARACTER of its content, each pairing the corresponding word
  // position in each direction (see Crossing's doc comment) -- so a
  // multi-character rebus crossing is never represented as a single
  // wide-span Crossing, only as several ordinary ones.
  void ComputeCrossings();

  int width_;
  int height_;
  std::vector<bool> blocked_;   // size width_ * height_
  std::vector<char> prefilled_;  // size width_ * height_, '\0' = unseeded
  std::vector<std::string> rebus_content_;  // size width_ * height_, "" = not a rebus cell

  std::vector<Slot> slots_;
  std::vector<Crossing> crossings_;

  // Per-cell lookup of which slot (if any) covers it in each direction and
  // the offset within that slot. Built by ComputeSlots and consumed by
  // ComputeCrossings.
  std::vector<int> cell_across_slot_;
  std::vector<int> cell_across_offset_;
  std::vector<int> cell_down_slot_;
  std::vector<int> cell_down_offset_;
};

}  // namespace xfill
