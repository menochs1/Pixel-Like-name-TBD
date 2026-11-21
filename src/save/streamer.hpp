#pragma once

#include <string>

#include "core/simulator.hpp"
#include "core/world.hpp"
#include "save/chunk_store.hpp"
#include "world/world_gen.hpp"

namespace px {

/// Keeps the chunks around the player in memory and the rest on disk.
///
/// A chunk that is not loaded comes back exactly as it left: from the store
/// if it was ever touched, otherwise regenerated from the seed (or empty, in a
/// world without a generator), which gives the identical chunk, so untouched
/// chunks never cost disk space. Unloading writes a chunk to the store only if
/// it was touched since it was loaded or generated.
///
/// To the simulation an unloaded chunk is a wall, so keep the loaded box at
/// least one chunk larger than the simulated region on every side: then every
/// cell that simulates, and every wake-up it sends, lands in a loaded chunk,
/// and a streamed world evolves exactly like one held fully in memory (tested).
class ChunkStreamer {
 public:
  /// `generator` may be null (missing chunks are empty); both it and `store`
  /// must outlive the streamer.
  ChunkStreamer(World& world, ChunkStore& store, const WorldGenerator* generator);

  struct Stats {
    int loaded = 0;     // chunks brought into memory this call
    int unloaded = 0;   // chunks taken out of memory
    int written = 0;    // of those, written to the store (touched)
  };

  /// Loads every chunk of `required` now, then up to `budget` chunks of
  /// `wanted` (nearest to the centre of `required` first), and unloads every
  /// loaded chunk outside `keep`. Boxes wrap on a wrapping world and are
  /// clipped otherwise. Give `keep` a margin beyond `wanted` so chunks at the
  /// edge are not loaded and unloaded over and over as the camera moves.
  Stats update(const ChunkRegion& required, const ChunkRegion& wanted, const ChunkRegion& keep, int budget);

  /// Loads one chunk if it is not loaded. Returns false (and records the
  /// error) if its stored data was unreadable; it is then regenerated.
  bool load(int cx, int cy);
  /// Unloads one chunk, writing it to the store if touched. Returns true if written.
  bool unload(int cx, int cy);
  void unload_all();

  void set_generator(const WorldGenerator* generator) { generator_ = generator; }
  const WorldGenerator* generator() const { return generator_; }
  /// The most recent load or write problem, if any (empty when none).
  const std::string& last_error() const { return last_error_; }

 private:
  bool in_box(int cx, int cy, const ChunkRegion& box) const;
  int wrap_cx(int cx) const;

  World& world_;
  ChunkStore& store_;
  const WorldGenerator* generator_;
  std::string last_error_;
};

}  // namespace px
