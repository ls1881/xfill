#include "xfill/grid.hpp"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace xfill {

namespace {

RebusEntry ParseRebusLine(const std::string& line) {
  size_t colon = line.find(':');
  if (colon == std::string::npos) {
    throw std::invalid_argument("malformed rebus entry (expected \"row,col:CONTENT\"): " + line);
  }
  std::string coords = line.substr(0, colon);
  size_t comma = coords.find(',');
  if (comma == std::string::npos) {
    throw std::invalid_argument("malformed rebus entry (expected \"row,col:CONTENT\"): " + line);
  }
  RebusEntry entry;
  try {
    entry.row = std::stoi(coords.substr(0, comma));
    entry.col = std::stoi(coords.substr(comma + 1));
  } catch (const std::exception&) {
    throw std::invalid_argument("malformed rebus entry (row/col not integers): " + line);
  }
  entry.content = line.substr(colon + 1);
  return entry;
}

}  // namespace

Grid::Grid(int width, int height)
    : width_(width),
      height_(height),
      blocked_(static_cast<size_t>(width) * static_cast<size_t>(height),
               false),
      prefilled_(static_cast<size_t>(width) * static_cast<size_t>(height),
                 '\0'),
      rebus_content_(static_cast<size_t>(width) * static_cast<size_t>(height)) {}

Grid Grid::FromSpec(const std::vector<std::string>& rows, const std::vector<RebusEntry>& rebus) {
  if (rows.empty()) {
    throw std::invalid_argument("grid spec must have at least one row");
  }
  int height = static_cast<int>(rows.size());
  int width = static_cast<int>(rows[0].size());
  Grid grid(width, height);

  for (int r = 0; r < height; ++r) {
    if (static_cast<int>(rows[static_cast<size_t>(r)].size()) != width) {
      throw std::invalid_argument("all rows must have equal width");
    }
    for (int c = 0; c < width; ++c) {
      char ch = rows[static_cast<size_t>(r)][static_cast<size_t>(c)];
      size_t cell = static_cast<size_t>(r) * static_cast<size_t>(width) +
                    static_cast<size_t>(c);
      grid.blocked_[cell] = (ch == '#');
      char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      if (upper >= 'A' && upper <= 'Z') {
        grid.prefilled_[cell] = upper;
      }
    }
  }

  // Rebus entries are a purely additive overlay on top of the primary
  // rows just parsed above: each one upgrades a single already-prefilled
  // cell to hold more than its first-letter stand-in. Validated strictly
  // (in bounds, not blocked, pure A-Z, first letter matching the row's
  // own stand-in character) since a malformed entry here would otherwise
  // silently corrupt every downstream word-length/letter-mask computation
  // that assumes rebus_content_ entries are trustworthy.
  for (const RebusEntry& entry : rebus) {
    if (entry.row < 0 || entry.row >= height || entry.col < 0 || entry.col >= width) {
      throw std::invalid_argument("rebus entry out of bounds: " + std::to_string(entry.row) +
                                   "," + std::to_string(entry.col));
    }
    size_t cell = static_cast<size_t>(entry.row) * static_cast<size_t>(width) +
                  static_cast<size_t>(entry.col);
    if (grid.blocked_[cell]) {
      throw std::invalid_argument("rebus entry at a blocked cell: " + std::to_string(entry.row) +
                                   "," + std::to_string(entry.col));
    }
    if (!grid.rebus_content_[cell].empty()) {
      throw std::invalid_argument("duplicate rebus entry for cell: " + std::to_string(entry.row) +
                                   "," + std::to_string(entry.col));
    }
    std::string content = entry.content;
    for (char& ch : content) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (content.empty()) {
      throw std::invalid_argument("rebus entry has empty content: " + std::to_string(entry.row) +
                                   "," + std::to_string(entry.col));
    }
    for (char ch : content) {
      if (ch < 'A' || ch > 'Z') {
        throw std::invalid_argument("rebus content must be A-Z after uppercasing: " + entry.content);
      }
    }
    if (content[0] != grid.prefilled_[cell]) {
      throw std::invalid_argument(
          "rebus content's first letter must match the grid row's character at row " +
          std::to_string(entry.row) + ", col " + std::to_string(entry.col));
    }
    grid.rebus_content_[cell] = content;
  }

  grid.ComputeSlots();
  grid.ComputeCrossings();
  return grid;
}

Grid Grid::FromFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("could not open grid file: " + path);

  std::vector<std::string> rows;
  std::vector<RebusEntry> rebus;
  std::string line;
  // The first blank line after at least one grid row has been read ends
  // the grid-row section; everything after it is a rebus entry. A blank
  // line before any row (or a stray blank line within the rebus section
  // itself) is just tolerated/skipped, matching this function's original,
  // more limited "skip blank/trailing lines" behavior -- so any grid-spec
  // file with no rebus section parses exactly as it always has.
  bool in_rebus_section = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!in_rebus_section) {
      if (line.empty()) {
        if (!rows.empty()) in_rebus_section = true;
        continue;
      }
      rows.push_back(line);
    } else {
      if (line.empty()) continue;
      rebus.push_back(ParseRebusLine(line));
    }
  }
  return FromSpec(rows, rebus);
}

void Grid::ComputeSlots() {
  size_t num_cells =
      static_cast<size_t>(width_) * static_cast<size_t>(height_);
  cell_across_slot_.assign(num_cells, -1);
  cell_across_offset_.assign(num_cells, -1);
  cell_down_slot_.assign(num_cells, -1);
  cell_down_offset_.assign(num_cells, -1);

  int next_id = 0;

  // Across slots: scan each row left to right.
  for (int r = 0; r < height_; ++r) {
    int c = 0;
    while (c < width_) {
      if (IsBlocked(r, c)) {
        ++c;
        continue;
      }
      int start = c;
      while (c < width_ && !IsBlocked(r, c)) ++c;
      int run = c - start;
      if (run >= 2) {
        Slot slot;
        slot.id = next_id++;
        slot.dir = Direction::Across;
        slot.row = r;
        slot.col = start;
        int char_pos = 0;
        for (int k = 0; k < run; ++k) {
          int cell = r * width_ + (start + k);
          slot.cells.push_back(cell);
          int clen = static_cast<int>(rebus_content_[static_cast<size_t>(cell)].size());
          if (clen == 0) clen = 1;
          slot.cell_lengths.push_back(clen);
          slot.cell_char_start.push_back(char_pos);
          char_pos += clen;
          cell_across_slot_[static_cast<size_t>(cell)] = slot.id;
          cell_across_offset_[static_cast<size_t>(cell)] = k;
        }
        slot.length = char_pos;
        slots_.push_back(std::move(slot));
      }
    }
  }

  // Down slots: scan each column top to bottom.
  for (int c = 0; c < width_; ++c) {
    int r = 0;
    while (r < height_) {
      if (IsBlocked(r, c)) {
        ++r;
        continue;
      }
      int start = r;
      while (r < height_ && !IsBlocked(r, c)) ++r;
      int run = r - start;
      if (run >= 2) {
        Slot slot;
        slot.id = next_id++;
        slot.dir = Direction::Down;
        slot.row = start;
        slot.col = c;
        int char_pos = 0;
        for (int k = 0; k < run; ++k) {
          int cell = (start + k) * width_ + c;
          slot.cells.push_back(cell);
          int clen = static_cast<int>(rebus_content_[static_cast<size_t>(cell)].size());
          if (clen == 0) clen = 1;
          slot.cell_lengths.push_back(clen);
          slot.cell_char_start.push_back(char_pos);
          char_pos += clen;
          cell_down_slot_[static_cast<size_t>(cell)] = slot.id;
          cell_down_offset_[static_cast<size_t>(cell)] = k;
        }
        slot.length = char_pos;
        slots_.push_back(std::move(slot));
      }
    }
  }
}

void Grid::ComputeCrossings() {
  crossings_.clear();
  size_t num_cells =
      static_cast<size_t>(width_) * static_cast<size_t>(height_);
  for (size_t cell = 0; cell < num_cells; ++cell) {
    int a = cell_across_slot_[cell];
    int d = cell_down_slot_[cell];
    if (a == -1 || d == -1) continue;
    const Slot& slot_a = slots_[static_cast<size_t>(a)];
    const Slot& slot_d = slots_[static_cast<size_t>(d)];
    int k_a = cell_across_offset_[cell];
    int k_d = cell_down_offset_[cell];
    int start_a = slot_a.cell_char_start[static_cast<size_t>(k_a)];
    int start_d = slot_d.cell_char_start[static_cast<size_t>(k_d)];
    // Both directions' cell_lengths at this shared cell are always equal
    // (they're both derived from this same cell's rebus_content_), so
    // there's exactly one span length to decompose, not two to reconcile.
    int span = slot_a.cell_lengths[static_cast<size_t>(k_a)];
    // An ordinary (non-rebus) cell decomposes into exactly the same
    // single Crossing this loop always produced -- span == 1 here in
    // that case, so this is a strict generalization, not a behavior
    // change, for every existing non-rebus grid. A rebus cell instead
    // contributes one Crossing per character of its content, each
    // pairing the corresponding word position in both directions: this
    // keeps Solver::Propagate's crossing-narrowing loop (see solver.cpp)
    // completely unchanged, since every Crossing it ever sees still
    // constrains exactly one word position against exactly one other.
    // "substring A[a:a+N] == substring B[b:b+N]" is logically equivalent
    // to "for all i in [0,N): A[a+i] == B[b+i]", so this decomposition
    // can never accept an invalid final assignment -- see Crossing's own
    // doc comment for the (minor, deliberate) propagation-tightness
    // tradeoff this makes.
    for (int i = 0; i < span; ++i) {
      crossings_.push_back(Crossing{a, start_a + i, d, start_d + i});
    }
  }
}

}  // namespace xfill
