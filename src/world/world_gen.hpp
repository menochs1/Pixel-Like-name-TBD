#pragma once

#include <cstdint>

#include "core/material.hpp"
#include "core/world.hpp"

namespace px {

enum class Biome : std::uint8_t { Desert, Meadow, Highlands };

/// Tunables for world generation. Depths and heights are fractions of the
/// world height unless noted, so the same parameters work at any world size.
struct WorldGenParams {
  float surface = 0.24f;         // mean ground level (0 = top of the world)
  float sea_level_offset = 0.03f;  // valleys this far below the mean ground level flood
  float cave_start = 14.0f;      // cells below the soil before caves begin
  float lava_depth = 0.74f;      // pools below this are lava
  float oil_depth = 0.46f;       // pools between this and lava_depth may be oil
  int bedrock_rows = 12;         // solid stone band at the bottom (no caves)
};

/// Seeded, deterministic terrain generator for a world `width` x `height` cells.
///
/// Every cell is a pure function of (seed, x, y): there is no global pass, no
/// flood fill and no state carried from one chunk to the next. Any chunk can be
/// generated on its own, in any order or in parallel, and comes out identical;
/// that is what save/load and chunk streaming will build on.
///
/// All noise repeats exactly every `width` cells in x, so terrain, caves and
/// biomes join up seamlessly across the seam of a wrapping world.
///
/// Layout, top to bottom: sky; a surface of rolling hills whose shape and soil
/// depend on the biome (desert dunes of sand, grassy meadows with trees and
/// lakes, bare rocky highlands); stone with coal seams, dirt and sand pockets;
/// tunnels and caverns that grow larger with depth, some holding pools of
/// water, oil or (deepest) lava; and a solid band at the bottom.
class WorldGenerator {
 public:
  WorldGenerator(const BaseMaterials& ids, std::uint64_t seed, int width, int height, WorldGenParams params = {});

  /// Material of one cell. Pure: the same arguments always give the same answer.
  MaterialId material_at(int x, int y) const;
  /// First solid row of the ground in column x (before caves).
  int surface_y(int x) const;
  Biome biome_at(int x) const;
  /// Rows above this (smaller y) are dry; ground below it is flooded up to it.
  int sea_level() const { return sea_y_; }

  /// Writes chunk (cx, cy) of `world`, waking only cells that may move or
  /// change. Touches no other chunk, so different chunks may be generated
  /// from different threads at once.
  void generate_chunk(World& world, int cx, int cy) const;
  /// Generates every chunk, spread over `threads` threads (0 = hardware count).
  /// The world must be width x height cells.
  void generate(World& world, int threads = 0) const;

 private:
  struct Column {
    int surface;     // first ground row
    int soil;        // rows of soil (dirt or sand) below the surface
    Biome biome;
    bool grass;      // top of the soil is grass
    bool entrance;   // tunnels may break through to the surface here
  };
  Column column(int x) const;
  MaterialId cell(int x, int y, const Column& col) const;
  MaterialId tree_cell(int x, int y) const;  // wood/leaves above ground, else kEmpty
  MaterialId underground(int x, int y, int depth, const Column& col) const;
  struct Bowl {
    enum Kind : std::uint8_t { None, Air, Liquid, Shell } kind = None;
    MaterialId liquid = kEmpty;
  };
  Bowl bowl_at(int x, int y) const;  // which part of a pool, if any, (x, y) is in
  float biome_value(int x) const;
  int wrap_dx(int dx) const;  // shortest signed x distance on the ring

  BaseMaterials ids_;
  std::uint64_t seed_;
  int width_, height_;
  WorldGenParams p_;
  int sea_y_;
  int tree_count_;   // trees are placed in this many equal slots around the ring
  int pool_cols_;    // liquid pools come from a coarse grid: this many columns...
  int pool_rows_;    // ...and rows
};

/// Convenience: generates `world` from its own seed with default parameters.
void generate_world(World& world, const BaseMaterials& ids, WorldGenParams params = {});

}  // namespace px
