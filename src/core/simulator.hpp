#pragma once

#include <cstdint>

#include "core/world.hpp"

namespace px {

struct StepStats {
  int active_chunks = 0;        // chunks with a non-empty dirty rect this step
  std::int64_t cells_scanned = 0;  // cells inside dirty rects
  std::int64_t cells_moved = 0;    // successful moves/swaps
  int paused_chunks = 0;           // chunks with pending work outside the region
};

/// Advances the world by one fixed step (intended for 60 steps per second).
///
/// Only chunks with a dirty rect are visited, and only the cells inside that rect.
/// Rows are processed bottom-to-top so falling material moves once per step; the
/// horizontal scan direction alternates every step to avoid left/right bias.
/// Every random choice comes from cell_hash(seed, x, y, tick), so the same seed
/// and inputs always produce the same world.
StepStats step(World& world);

/// A box of chunks, inclusive, in chunk coordinates. On a wrapping world the
/// x range may run past either edge and wraps around.
struct ChunkRegion {
  int cx0 = 0, cy0 = 0, cx1 = -1, cy1 = -1;
};

/// Like step(), but only simulates chunks inside `region` (e.g. around the
/// camera). Chunks outside are paused, not lost: their pending work is kept,
/// keeps collecting wake-ups from active neighbours, and resumes as soon as they
/// are inside the region again. This bounds the cost per step by the region
/// size, however much is happening elsewhere in a large world.
StepStats step(World& world, const ChunkRegion& region);

}  // namespace px
