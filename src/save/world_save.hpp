#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/world.hpp"
#include "world/world_gen.hpp"

namespace px {

class ChunkStore;

/// What a save records besides the cells.
struct SaveInfo {
  /// How untouched chunks are recreated: generated from the world's seed with
  /// these parameters, or (nullopt) left empty.
  std::optional<WorldGenParams> generator;
  int view_x = 0, view_y = 0;  // where the camera was (a hint for the app)
  bool has_player = false;     // where the player stood (top-left of its box)
  float player_x = 0.0f, player_y = 0.0f;
};

/// A save is a directory:
///
///   world.pxw            header: format version, size, edges, seed, step
///                        counter, SaveInfo, and the material table (names of
///                        authored materials, recipes of derived ones)
///   chunks/c_X_Y.pxc     one file per chunk that differs from what the
///                        generator (or emptiness) would give
///
/// Only touched chunks are written, so a freshly generated world saves as a
/// few kilobytes. Materials are saved by name and derived materials by their
/// recipe (VariantRecord), so a save stays loadable when materials are added
/// or reordered.
///
/// Writes `world` to `dir`. Loaded chunks come from memory; chunks that are not
/// loaded come from `store` (may be null for a fully loaded world). The new save
/// is written beside `dir` and then swapped in, so a failure never leaves a
/// half-written save, and `dir` may be the save `store` was loaded from.
bool save_world(const World& world, const SaveInfo& info, const std::filesystem::path& dir, const ChunkStore* store,
                std::string* error = nullptr);

/// After save_world() with a store: makes that save the store's base and marks
/// every loaded chunk untouched, since all of it is now in the save.
void mark_saved(World& world, ChunkStore& store, const std::filesystem::path& dir);

/// An opened save: a World with no chunks loaded, ready for a ChunkStreamer.
struct OpenedSave {
  std::unique_ptr<World> world;
  SaveInfo info;
  std::filesystem::path chunk_dir;
  std::vector<MaterialId> remap;  // material ids as saved -> ids in the registry
};

/// Reads a save's header and material table, adding any derived materials it
/// needs to `materials`. Fails (nullopt, `error` set) if the save is missing,
/// damaged, or uses a material the registry does not have.
std::optional<OpenedSave> open_save(const std::filesystem::path& dir, MaterialRegistry& materials,
                                    std::string* error = nullptr);

/// Loads a whole save into a fully loaded World (for small worlds, tools and
/// tests). Untouched chunks are regenerated with `ids` when the save has a
/// generator.
std::unique_ptr<World> load_world(const std::filesystem::path& dir, MaterialRegistry& materials,
                                  const BaseMaterials& ids, std::string* error = nullptr);

}  // namespace px
