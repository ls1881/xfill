#include "xfill/grid.hpp"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace xfill {

Grid::Grid(int width, int height)
    : width_(width),
      height_(height),
      blocked_(static_cast<size_t>(width) * static_cast<size_t>(height),
               false),
      prefilled_(static_cast<size_t>(width) * static_cast<size_t>(height),
                 '\0') {}

Grid Grid::FromSpec(const std::vector<std::string>& rows) {
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

  grid.ComputeSlots();
  grid.ComputeCrossings();
  return grid;
}

Grid Grid::FromFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("could not open grid file: " + path);

  std::vector<std::string> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;  // skip blank/trailing lines
    rows.push_back(line);
  }
  return FromSpec(rows);
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
      int length = c - start;
      if (length >= 2) {
        Slot slot;
        slot.id = next_id++;
        slot.dir = Direction::Across;
        slot.row = r;
        slot.col = start;
        slot.length = length;
        for (int k = 0; k < length; ++k) {
          int cell = r * width_ + (start + k);
          slot.cells.push_back(cell);
          cell_across_slot_[static_cast<size_t>(cell)] = slot.id;
          cell_across_offset_[static_cast<size_t>(cell)] = k;
        }
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
      int length = r - start;
      if (length >= 2) {
        Slot slot;
        slot.id = next_id++;
        slot.dir = Direction::Down;
        slot.row = start;
        slot.col = c;
        slot.length = length;
        for (int k = 0; k < length; ++k) {
          int cell = (start + k) * width_ + c;
          slot.cells.push_back(cell);
          cell_down_slot_[static_cast<size_t>(cell)] = slot.id;
          cell_down_offset_[static_cast<size_t>(cell)] = k;
        }
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
    if (a != -1 && d != -1) {
      crossings_.push_back(Crossing{a, cell_across_offset_[cell], d,
                                     cell_down_offset_[cell]});
    }
  }
}

}  // namespace xfill
