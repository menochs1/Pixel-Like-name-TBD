#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/chunk.hpp"
#include "core/material.hpp"

namespace px {

/// How the left and right edges of the world behave.
enum class EdgeX : std::uint8_t {
  Wall,  // cells beyond the left/right edge are an indestructible wall
  Wrap,  // x wraps around: walking off the right edge comes back in on the left
};

/// A grid of chunks. Coordinates are in cells, y grows downward. The top and
/// bottom are always an indestructible wall; the sides are a wall or wrap
/// around (EdgeX::Wrap), making the world a cylinder.
///
/// With wrapping, every accessor takes any x and maps it into [0, width), so
/// simulation code can look at x - 1 or x + 8 without special cases. Chunk
/// boundaries line up with the seam (width is a whole number of chunks), so
/// local coordinates never need wrapping, only chunk indices.
///
/// The world holds a pointer to its MaterialRegistry; the registry must outlive it.
/// The simulation may add derived materials to it while stepping.
/// World is pure data: it has no knowledge of rendering.
class World {
 public:
  World(int width_chunks, int height_chunks, MaterialRegistry& materials, std::uint64_t seed,
        EdgeX edge_x = EdgeX::Wall);

  int width() const { return width_; }
  int height() const { return height_; }
  int width_chunks() const { return width_chunks_; }
  int height_chunks() const { return height_chunks_; }
  std::uint64_t seed() const { return seed_; }
  std::uint32_t tick() const { return tick_; }
  const MaterialRegistry& materials() const { return *materials_; }
  MaterialRegistry& materials() { return *materials_; }
  bool wraps_x() const { return wrap_x_; }

  /// Maps x into [0, width) on a wrapping world; returns it unchanged otherwise.
  int wrap_x(int x) const {
    if (!wrap_x_ || static_cast<unsigned>(x) < static_cast<unsigned>(width_)) return x;
    const int m = x % width_;
    return m < 0 ? m + width_ : m;
  }

  /// True if (x, y) is a real cell. On a wrapping world every x is.
  bool in_bounds(int x, int y) const {
    return static_cast<unsigned>(y) < static_cast<unsigned>(height_) &&
           (wrap_x_ || static_cast<unsigned>(x) < static_cast<unsigned>(width_));
  }

  /// Material at (x, y), or kOutOfBounds outside the world.
  MaterialId get(int x, int y) const {
    if (!in_bounds(x, y)) return kOutOfBounds;
    return chunk_for(x, y).material[local_index(x, y)];
  }

  /// Places a material, resets its velocity, picks a colour shade and wakes the area.
  /// Temperature becomes the material's spawn temperature (ambient if it has none)
  /// and limited-life materials get a fresh lifetime. Out-of-bounds writes are ignored.
  void set(int x, int y, MaterialId material);

  Temperature temperature(int x, int y) const {
    return in_bounds(x, y) ? chunk_for(x, y).temperature(local_index(x, y)) : kAmbient;
  }
  /// Clamped to the storable range, Chunk::kMinTemp..Chunk::kMaxTemp.
  void set_temperature(int x, int y, int t);

  /// Share of the cell's durability left, 0..255 (255 = intact, also outside the world).
  int health(int x, int y) const { return in_bounds(x, y) ? chunk_for(x, y).health[local_index(x, y)] : 255; }
  /// Deals `amount` damage, in the same units as MaterialDef::durability. A cell
  /// whose health runs out breaks (becomes empty); a material with durability 0
  /// breaks at once. Empty cells and cells outside the world take no damage.
  /// Returns true if the cell broke.
  bool damage(int x, int y, int amount);

  void fill_rect(int x, int y, int w, int h, MaterialId material);
  /// Fills a disc. `coverage` in (0, 1] places material in that fraction of cells
  /// (deterministically), which looks more natural for powders and liquids.
  void fill_circle(int cx, int cy, int radius, MaterialId material, float coverage = 1.0f);
  void clear();

  /// Marks (x, y) and its 8 neighbours for update on the next step.
  void wake(int x, int y);
  /// Marks the (2r+1)x(2r+1) box around (x, y), like wake() with a larger radius.
  void wake_area(int x, int y, int r);
  void wake_all();
  /// Marks just (x, y) for update this step (if not yet passed) and next step.
  void wake_cell(int x, int y) {
    Chunk& c = chunk_for(x, y);
    c.dirty.include(x & kChunkMask, y & kChunkMask);
    c.next_dirty.include(x & kChunkMask, y & kChunkMask);
  }
  /// Keeps just (x, y) active for the next step (a cell that is still changing).
  void keep_awake(int x, int y) { chunk_for(x, y).next_dirty.include(x & kChunkMask, y & kChunkMask); }

  Chunk& chunk(int cx, int cy) { return chunks_[static_cast<std::size_t>(cy * width_chunks_ + cx)]; }
  const Chunk& chunk(int cx, int cy) const { return chunks_[static_cast<std::size_t>(cy * width_chunks_ + cx)]; }
  /// Chunk holding cell (x, y). x may be outside [0, width) on a wrapping world.
  Chunk& chunk_for(int x, int y) { return chunk(chunk_x(x), y >> kChunkShift); }
  const Chunk& chunk_for(int x, int y) const { return chunk(chunk_x(x), y >> kChunkShift); }
  /// Index within its chunk. Correct for any x: the width is a whole number of
  /// chunks, so x and its wrapped value share the same low bits.
  static int local_index(int x, int y) { return Chunk::index(x & kChunkMask, y & kChunkMask); }

  /// Chunks that will be simulated on the next step.
  int active_chunk_count() const;
  /// Number of cells holding `material` (linear scan; for tests and tools).
  std::size_t count(MaterialId material) const;

  /// Called by the simulator at the start of each step.
  void advance_tick() { ++tick_; }

 private:
  /// Chunk column of cell x, wrapped on a wrapping world.
  int chunk_x(int x) const {
    const int cx = x >> kChunkShift;  // arithmetic shift: floor division, also for negative x
    if (static_cast<unsigned>(cx) < static_cast<unsigned>(width_chunks_)) return cx;
    const int m = cx % width_chunks_;
    return m < 0 ? m + width_chunks_ : m;
  }

  int width_chunks_;
  int height_chunks_;
  int width_;
  int height_;
  MaterialRegistry* materials_;
  std::uint64_t seed_;
  std::uint32_t tick_ = 0;
  bool wrap_x_;
  std::vector<Chunk> chunks_;
};

}  // namespace px
