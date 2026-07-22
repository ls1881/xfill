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

  // Load a grid layout from a text spec: '.' = open cell, '#' = block.
  static Grid FromSpec(const std::vector<std::string>& rows);

  int width() const { return width_; }
  int height() const { return height_; }
  const std::vector<Slot>& slots() const { return slots_; }
  const std::vector<Crossing>& crossings() const { return crossings_; }

 private:
  void ComputeSlots();
  void ComputeCrossings();

  int width_;
  int height_;
  std::vector<bool> blocked_;  // size width_ * height_
  std::vector<Slot> slots_;
  std::vector<Crossing> crossings_;
};

}  // namespace xfill
