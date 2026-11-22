#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/chunk.hpp"
#include "core/material.hpp"

namespace px {

/// How the left and right edges of the world behave.
enum class EdgeX : std::uint8_t {
  Wall,  // cells beyond the left/right edge are an indestructible wall
  Wrap,  // x wraps around: walking off the right edge comes back in on the left
};

/// The climate: temperature with altitude, the wind, and the regional weather
/// (core/weather.hpp). The settings are part of the world's rules, not of its
/// data: saves do not store them. All off by default.
struct Weather {
  /// Mean ground level. Above it the air gets colder (lapse_rate) and the wind
  /// builds to full strength 120 rows up; at and below it, it is 20 C and calm.
  /// 0: no ground level (the same temperature and wind everywhere).
  int ground_y = 0;
  /// Degrees colder per row above ground_y: the temperature everything rests
  /// at up there (ambient_at). 0.03 freezes ~730 rows up.
  float lapse_rate = 0.0f;
  /// Vapour (steam) rising above this row leaves the world and becomes moisture
  /// in the weather overhead, out of sight. 0: never.
  int vapour_y = 0;
  /// Weather areas: the world is cut into columns this wide, each with its own
  /// moisture, which builds up by itself (humidity_rate per step, in cells of
  /// water), drifts with the wind, and is fed by vapour. At rain_start an area
  /// starts raining, about rain_per_step drops per step, until it is down to
  /// rain_stop. 0 areas: no rain.
  int region_width = 0;
  float humidity_rate = 0.0f;
  float rain_start = 2000.0f;
  float rain_stop = 300.0f;
  float rain_per_step = 3.0f;
  /// Wind, in cells per step (0.35 = 21 cells/s). It wanders between
  /// -max_wind and +max_wind over about `wind_period` steps, plus `wind_bias`
  /// (e.g. set by the player). Deterministic: a function of seed and step.
  float max_wind = 0.0f;
  int wind_period = 3600;
  float wind_bias = 0.0f;
};

/// Whether a world starts with every chunk in memory, or with none so that a
/// ChunkStreamer loads them around the player.
enum class ChunkStorage : std::uint8_t {
  AllLoaded,
  Streamed,
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
///
/// Chunks can be unloaded (ChunkStorage::Streamed). Cells of an unloaded chunk
/// are not in_bounds and read as kOutOfBounds, so to the simulation an
/// unloaded chunk is a solid wall; the streamer keeps a margin of loaded
/// chunks around everything that simulates, so it never meets one.
class World {
 public:
  World(int width_chunks, int height_chunks, MaterialRegistry& materials, std::uint64_t seed,
        EdgeX edge_x = EdgeX::Wall, ChunkStorage storage = ChunkStorage::AllLoaded);

  int width() const { return width_; }
  int height() const { return height_; }
  int width_chunks() const { return width_chunks_; }
  int height_chunks() const { return height_chunks_; }
  std::uint64_t seed() const { return seed_; }
  std::uint32_t tick() const { return tick_; }
  const MaterialRegistry& materials() const { return *materials_; }
  MaterialRegistry& materials() { return *materials_; }
  bool wraps_x() const { return wrap_x_; }
  const Weather& weather() const { return weather_; }
  /// Sets the climate and resets the weather areas (moisture starts at a
  /// different, seeded point of the cycle in each area).
  void set_weather(const Weather& weather);
  /// Changes the settings but keeps each area's moisture and rain.
  void tune_weather(const Weather& weather) {
    weather_ = weather;
    wind_tick_ = 0xFFFFFFFFU;
  }
  /// The temperature cells rest at in row y: 20 C at and below ground level,
  /// colder above it.
  int ambient_at(int y) const {
    if (weather_.lapse_rate <= 0.0f || y >= weather_.ground_y) return kAmbient;
    return std::max(-273, kAmbient - static_cast<int>(static_cast<float>(weather_.ground_y - y) * weather_.lapse_rate));
  }
  // Weather areas (see core/weather.hpp).
  int weather_areas() const { return static_cast<int>(moisture_.size()); }
  int weather_area(int x) const;  // the area over column x (0 if there are none)
  float& moisture(int area) { return moisture_[static_cast<std::size_t>(area)]; }
  float moisture(int area) const { return moisture_[static_cast<std::size_t>(area)]; }
  bool raining(int area) const { return raining_[static_cast<std::size_t>(area)] != 0; }
  void set_raining(int area, bool on) { raining_[static_cast<std::size_t>(area)] = on ? 1 : 0; }
  /// Vapour that left the world over column x.
  void add_moisture(int x, float amount) {
    if (!moisture_.empty()) moisture_[static_cast<std::size_t>(weather_area(x))] += amount;
  }
  /// The wind at row y this step, in cells per step (positive blows right).
  float wind_at(int y) const;

  /// Maps x into [0, width) on a wrapping world; returns it unchanged otherwise.
  int wrap_x(int x) const {
    if (!wrap_x_ || static_cast<unsigned>(x) < static_cast<unsigned>(width_)) return x;
    const int m = x % width_;
    return m < 0 ? m + width_ : m;
  }

  /// True if (x, y) is a cell of a loaded chunk. On a wrapping world any x can be.
  bool in_bounds(int x, int y) const { return cell_chunk(x, y) != nullptr; }

  /// Material at (x, y), or kOutOfBounds outside the world or in an unloaded chunk.
  MaterialId get(int x, int y) const {
    const Chunk* c = cell_chunk(x, y);
    return c ? c->material[local_index(x, y)] : kOutOfBounds;
  }

  /// Places a material, resets its velocity, picks a colour shade and wakes the area.
  /// Temperature becomes the material's spawn temperature (ambient if it has none)
  /// and limited-life materials get a fresh lifetime. Out-of-bounds writes are ignored.
  void set(int x, int y, MaterialId material);

  Temperature temperature(int x, int y) const {
    const Chunk* c = cell_chunk(x, y);
    return c ? c->temperature(local_index(x, y)) : kAmbient;
  }
  /// Clamped to the storable range, Chunk::kMinTemp..Chunk::kMaxTemp.
  void set_temperature(int x, int y, int t);

  /// Share of the cell's durability left, 0..255 (255 = intact, also outside the world).
  int health(int x, int y) const {
    const Chunk* c = cell_chunk(x, y);
    return c ? c->health[local_index(x, y)] : 255;
  }
  /// Deals `amount` damage, in the same units as MaterialDef::durability. A cell
  /// whose health runs out breaks (becomes empty); a material with durability 0
  /// breaks at once. Empty cells and cells outside the world take no damage.
  /// Returns true if the cell broke.
  bool damage(int x, int y, int amount);

  void fill_rect(int x, int y, int w, int h, MaterialId material);
  /// Fills a disc. `coverage` in (0, 1] places material in that fraction of cells
  /// (deterministically), which looks more natural for powders and liquids.
  void fill_circle(int cx, int cy, int radius, MaterialId material, float coverage = 1.0f);
  /// Empties every loaded chunk (marking them touched, so the emptiness is saved).
  void clear();
  /// An empty chunk for chunk row cy, its air at the temperature of its
  /// altitude (ambient_at), so it is at rest from the start.
  std::unique_ptr<Chunk> make_empty_chunk(int cy) const;
  /// Puts every loaded cell that has no temperature of its own (no spawn
  /// temperature) at its altitude's temperature. For worlds built by hand
  /// after set_weather() (generated worlds already start that way).
  void settle_temperatures();

  /// Marks (x, y) and its 8 neighbours for update on the next step.
  void wake(int x, int y);
  /// Marks the (2r+1)x(2r+1) box around (x, y), like wake() with a larger radius.
  void wake_area(int x, int y, int r);
  void wake_all();
  /// Marks just (x, y) for update this step (if not yet passed) and next step.
  void wake_cell(int x, int y) {
    Chunk& c = chunk_for(x, y);
    if (stepping_ && c.dirty.empty()) note_active(chunk_x(x), y >> kChunkShift);
    c.dirty.include(x & kChunkMask, y & kChunkMask);
    c.next_dirty.include(x & kChunkMask, y & kChunkMask);
    c.touched = true;
  }
  /// Keeps just (x, y) active for the next step (a cell that is still changing).
  void keep_awake(int x, int y) {
    Chunk& c = chunk_for(x, y);
    c.next_dirty.include(x & kChunkMask, y & kChunkMask);
    c.touched = true;
  }

  /// The chunk at chunk coordinates (cx, cy); it must be loaded.
  Chunk& chunk(int cx, int cy) { return *chunks_[slot(cx, cy)]; }
  const Chunk& chunk(int cx, int cy) const { return *chunks_[slot(cx, cy)]; }
  /// The chunk at (cx, cy), or nullptr if it is not loaded.
  Chunk* chunk_ptr(int cx, int cy) { return chunks_[slot(cx, cy)].get(); }
  const Chunk* chunk_ptr(int cx, int cy) const { return chunks_[slot(cx, cy)].get(); }
  bool loaded(int cx, int cy) const { return chunks_[slot(cx, cy)] != nullptr; }
  int loaded_chunk_count() const;
  /// Puts `chunk` at (cx, cy), replacing whatever was there.
  Chunk& load_chunk(int cx, int cy, std::unique_ptr<Chunk> chunk);
  /// Takes chunk (cx, cy) out of the world (nullptr if it was not loaded).
  std::unique_ptr<Chunk> unload_chunk(int cx, int cy);

  /// Chunk holding cell (x, y), which must be loaded. x may be outside
  /// [0, width) on a wrapping world.
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
  /// Restores the step counter (loading a save): the simulation's random
  /// choices depend on it.
  void set_tick(std::uint32_t tick) { tick_ = tick; }

  // ---- bookkeeping for the simulator's scan (so a step costs what is active,
  // not what the world's size is)
  /// Slots (cy * width_chunks + cx) of every loaded chunk, in no particular order.
  const std::vector<int>& loaded_slots() const { return loaded_; }
  /// Between begin_step() and end_step(), wake-ups that give a chunk its first
  /// dirty cells this step add its column to that chunk row's active list.
  void begin_step();
  void end_step() { stepping_ = false; }
  /// Records chunk (cx, cy) as having dirty cells this step (sorted, no duplicates).
  void note_active(int cx, int cy);
  /// Columns of chunk row `cy` with dirty cells this step, ascending. May grow
  /// during the step; read it afresh rather than holding iterators.
  const std::vector<int>& active_columns(int cy) const { return active_rows_[static_cast<std::size_t>(cy)]; }

 private:
  std::size_t slot(int cx, int cy) const { return static_cast<std::size_t>(cy * width_chunks_ + cx); }
  /// The loaded chunk holding cell (x, y), or nullptr (outside or unloaded).
  const Chunk* cell_chunk(int x, int y) const {
    if (static_cast<unsigned>(y) >= static_cast<unsigned>(height_)) return nullptr;
    if (!wrap_x_ && static_cast<unsigned>(x) >= static_cast<unsigned>(width_)) return nullptr;
    return chunks_[slot(chunk_x(x), y >> kChunkShift)].get();
  }

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
  std::vector<std::unique_ptr<Chunk>> chunks_;  // nullptr = not loaded
  std::vector<int> loaded_;                     // slots of loaded chunks
  std::vector<std::vector<int>> active_rows_;   // per chunk row, during a step
  bool stepping_ = false;
  Weather weather_;
  std::vector<float> moisture_;         // per weather area
  std::vector<std::uint8_t> raining_;
  mutable float wind_base_ = 0.0f;               // wind before shelter, for wind_tick_
  mutable std::uint32_t wind_tick_ = 0xFFFFFFFFU;
};

}  // namespace px
