#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/material.hpp"

namespace px {

/// Encoded chunks on disk, keyed by chunk position. Two layers:
///
///  - the work directory, where chunks unloaded during this session are
///    written (owned by the store: created on construction, its files deleted
///    by clear() and the destructor);
///  - an optional read-only base: the chunks of the save that was loaded, whose
///    material ids go through a remap (ids as saved -> ids now).
///
/// Reads prefer the work directory, so a chunk changed since loading shadows
/// the saved one. Nothing in the base is ever modified.
class ChunkStore {
 public:
  explicit ChunkStore(std::filesystem::path work_dir);
  ~ChunkStore();
  ChunkStore(const ChunkStore&) = delete;
  ChunkStore& operator=(const ChunkStore&) = delete;

  void set_base(std::filesystem::path chunk_dir, std::vector<MaterialId> remap);
  /// Forgets everything: deletes the work files and drops the base.
  void clear();
  /// After a save to `chunk_dir` (whose ids are the registry's own): it becomes
  /// the base, and the work files, all now part of it, are deleted.
  void rebase(std::filesystem::path chunk_dir);

  bool contains(int cx, int cy) const;
  /// Reads chunk (cx, cy). `remap` is set to the remap its ids need, or to
  /// nullptr when they are the registry's own. False if not stored or unreadable.
  bool read(int cx, int cy, std::vector<std::uint8_t>& out, const std::vector<MaterialId>** remap) const;
  bool write(int cx, int cy, const std::vector<std::uint8_t>& bytes);
  /// Every stored position, base and work, without duplicates.
  std::vector<std::pair<int, int>> positions() const;
  std::size_t work_count() const { return work_.size(); }
  const std::filesystem::path& work_dir() const { return work_dir_; }

  static std::string file_name(int cx, int cy);

 private:
  std::filesystem::path work_dir_;
  std::filesystem::path base_dir_;
  std::vector<MaterialId> base_remap_;
  std::set<std::pair<int, int>> work_, base_;
};

}  // namespace px
