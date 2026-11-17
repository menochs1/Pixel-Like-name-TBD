#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/chunk.hpp"
#include "core/material.hpp"

namespace px {

/// A bounded grid of chunks. Coordinates are in cells, y grows downward.
/// Cells outside the world behave as an indestructible wall.
///
/// The world holds a pointer to its MaterialRegistry; the registry must outlive it.
/// World is pure data: it has no knowledge of rendering.
class World {
 public:
  World(int width_chunks, int height_chunks, const MaterialRegistry& materials, std::uint64_t seed);

  int width() const { return width_; }
  int height() const { return height_; }
  int width_chunks() const { return width_chunks_; }
  int height_chunks() const { return height_chunks_; }
  std::uint64_t seed() const { return seed_; }
  std::uint32_t tick() const { return tick_; }
  const MaterialRegistry& materials() const { return *materials_; }

  bool in_bounds(int x, int y) const {
    return static_cast<unsigned>(x) < static_cast<unsigned>(width_) &&
           static_cast<unsigned>(y) < static_cast<unsigned>(height_);
  }

  /// Material at (x, y), or kOutOfBounds outside the world.
  MaterialId get(int x, int y) const {
    if (!in_bounds(x, y)) return kOutOfBounds;
    return chunk_for(x, y).material[local_index(x, y)];
  }

  /// Places a material, resets its velocity, picks a colour shade and wakes the area.
  /// Out-of-bounds writes are ignored.
  void set(int x, int y, MaterialId material);

  void fill_rect(int x, int y, int w, int h, MaterialId material);
  /// Fills a disc. `coverage` in (0, 1] places material in that fraction of cells
  /// (deterministically), which looks more natural for powders and liquids.
  void fill_circle(int cx, int cy, int radius, MaterialId material, float coverage = 1.0f);
  void clear();

  /// Marks (x, y) and its 8 neighbours for update on the next step.
  void wake(int x, int y);
  void wake_all();

  Chunk& chunk(int cx, int cy) { return chunks_[static_cast<std::size_t>(cy * width_chunks_ + cx)]; }
  const Chunk& chunk(int cx, int cy) const { return chunks_[static_cast<std::size_t>(cy * width_chunks_ + cx)]; }
  Chunk& chunk_for(int x, int y) { return chunk(x >> kChunkShift, y >> kChunkShift); }
  const Chunk& chunk_for(int x, int y) const { return chunk(x >> kChunkShift, y >> kChunkShift); }
  static int local_index(int x, int y) { return Chunk::index(x & kChunkMask, y & kChunkMask); }

  /// Chunks that will be simulated on the next step.
  int active_chunk_count() const;
  /// Number of cells holding `material` (linear scan; for tests and tools).
  std::size_t count(MaterialId material) const;

  /// Called by the simulator at the start of each step.
  void advance_tick() { ++tick_; }

 private:
  int width_chunks_;
  int height_chunks_;
  int width_;
  int height_;
  const MaterialRegistry* materials_;
  std::uint64_t seed_;
  std::uint32_t tick_ = 0;
  std::vector<Chunk> chunks_;
};

}  // namespace px
