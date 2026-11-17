#pragma once

#include <cstdint>

#include "core/world.hpp"

namespace px {

struct StepStats {
  int active_chunks = 0;        // chunks with a non-empty dirty rect this step
  std::int64_t cells_scanned = 0;  // cells inside dirty rects
  std::int64_t cells_moved = 0;    // successful moves/swaps
};

/// Advances the world by one fixed step (intended for 60 steps per second).
///
/// Only chunks with a dirty rect are visited, and only the cells inside that rect.
/// Rows are processed bottom-to-top so falling material moves once per step; the
/// horizontal scan direction alternates every step to avoid left/right bias.
/// Every random choice comes from cell_hash(seed, x, y, tick), so the same seed
/// and inputs always produce the same world.
StepStats step(World& world);

}  // namespace px
