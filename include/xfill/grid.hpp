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
  int length;
  // Cell indices (row * width + col) covered by this slot, in order.
  std::vector<int> cells;
};

// Which slots cross at a given cell, and at what offset within each.
struct Crossing {
  int slot_a;
  int offset_a;
  int slot_b;
  int offset_b;
};

class Grid {
 public:
  Grid(int width, int height);

  // Loads a grid layout from a text spec: '.' = open cell, '#' = block.
  static Grid FromSpec(const std::vector<std::string>& rows);

  // Reads a grid spec from a file, one row per line (trailing blank
  // lines are ignored; '\r' is stripped for files with CRLF endings).
  static Grid FromFile(const std::string& path);

  int width() const { return width_; }
  int height() const { return height_; }
  bool IsBlocked(int row, int col) const {
    return blocked_[static_cast<size_t>(row) * static_cast<size_t>(width_) +
                     static_cast<size_t>(col)];
  }

  const std::vector<Slot>& slots() const { return slots_; }
  const Slot& SlotById(int id) const {
    return slots_[static_cast<size_t>(id)];
  }
  const std::vector<Crossing>& crossings() const { return crossings_; }

 private:
  // Scans each row, then each column, for maximal runs of open cells;
  // any run of length >= 2 becomes a Slot (a length-1 run has nothing to
  // cross, so it can never be a real crossword entry).
  void ComputeSlots();
  // Any cell covered by both an across and a down slot is a Crossing
  // between them, at that cell's offset within each.
  void ComputeCrossings();

  int width_;
  int height_;
  std::vector<bool> blocked_;  // size width_ * height_

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
