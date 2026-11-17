#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "core/material.hpp"

namespace px {

inline constexpr int kChunkShift = 6;
inline constexpr int kChunkSize = 1 << kChunkShift;  // 64x64 cells
inline constexpr int kChunkMask = kChunkSize - 1;
inline constexpr int kChunkArea = kChunkSize * kChunkSize;

/// Inclusive bounding box of cells that need updating, in chunk-local coords.
struct DirtyRect {
  int min_x = kChunkSize, min_y = kChunkSize, max_x = -1, max_y = -1;

  bool empty() const { return max_x < min_x; }
  void reset() { *this = DirtyRect{}; }
  void include(int x, int y) {
    min_x = std::min(min_x, x);
    min_y = std::min(min_y, y);
    max_x = std::max(max_x, x);
    max_y = std::max(max_y, y);
  }
  void include_rect(int x0, int y0, int x1, int y1) {
    min_x = std::min(min_x, x0);
    min_y = std::min(min_y, y0);
    max_x = std::max(max_x, x1);
    max_y = std::max(max_y, y1);
  }
  int area() const { return empty() ? 0 : (max_x - min_x + 1) * (max_y - min_y + 1); }
};

/// A 64x64 block of cells stored as a structure of arrays. Each array is
/// indexed by (local_y << kChunkShift) | local_x.
///
/// Memory: 2 + 2 + 2 + 1 + 1 + 1 + 1 = 10 bytes per cell (~40 KiB per chunk),
/// so a 10-million-cell world is ~100 MiB. Health and state flags join as
/// further arrays in later roadmap steps.
struct Chunk {
  std::array<MaterialId, kChunkArea> material{};
  std::array<Temperature, kChunkArea> temperature{};  // degrees Celsius
  std::array<std::uint16_t, kChunkArea> life{};       // remaining steps; 0 = unlimited
  std::array<std::int8_t, kChunkArea> velocity_y{};   // cells/step falling speed
  std::array<std::int8_t, kChunkArea> velocity_x{};   // liquids: -1/0/+1 flow direction
  std::array<std::uint8_t, kChunkArea> shade{};       // 0..15 colour variant
  std::array<std::uint8_t, kChunkArea> clock{};       // last step this cell moved/updated

  DirtyRect dirty;       // being processed this step (can grow mid-step)
  DirtyRect next_dirty;  // collected this step, processed next step

  Chunk() { temperature.fill(kAmbient); }

  static constexpr int index(int local_x, int local_y) { return (local_y << kChunkShift) | local_x; }
};

}  // namespace px
