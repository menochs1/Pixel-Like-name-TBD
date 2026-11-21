#include "save/chunk_store.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace px {
namespace fs = std::filesystem;

namespace {

bool parse_name(const std::string& name, int& cx, int& cy) {
  return std::sscanf(name.c_str(), "c_%d_%d.pxc", &cx, &cy) == 2 && name == ChunkStore::file_name(cx, cy);
}

bool read_file(const fs::path& path, std::vector<std::uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

}  // namespace

std::string ChunkStore::file_name(int cx, int cy) { return "c_" + std::to_string(cx) + "_" + std::to_string(cy) + ".pxc"; }

ChunkStore::ChunkStore(fs::path work_dir) : work_dir_(std::move(work_dir)) {
  std::error_code ec;
  fs::create_directories(work_dir_, ec);
  for (const auto& entry : fs::directory_iterator(work_dir_, ec)) {  // leftovers: start clean
    fs::remove(entry.path(), ec);
  }
}

ChunkStore::~ChunkStore() {
  std::error_code ec;
  for (const auto& p : work_) fs::remove(work_dir_ / file_name(p.first, p.second), ec);
  fs::remove(work_dir_, ec);  // only succeeds if empty, which is fine
}

void ChunkStore::set_base(fs::path chunk_dir, std::vector<MaterialId> remap) {
  base_dir_ = std::move(chunk_dir);
  base_remap_ = std::move(remap);
  base_.clear();
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(base_dir_, ec)) {
    int cx = 0, cy = 0;
    if (parse_name(entry.path().filename().string(), cx, cy)) base_.insert({cx, cy});
  }
}

void ChunkStore::clear() {
  std::error_code ec;
  for (const auto& p : work_) fs::remove(work_dir_ / file_name(p.first, p.second), ec);
  work_.clear();
  base_.clear();
  base_dir_.clear();
  base_remap_.clear();
}

void ChunkStore::rebase(fs::path chunk_dir) {
  std::error_code ec;
  for (const auto& p : work_) fs::remove(work_dir_ / file_name(p.first, p.second), ec);
  work_.clear();
  set_base(std::move(chunk_dir), {});
}

bool ChunkStore::contains(int cx, int cy) const { return work_.count({cx, cy}) || base_.count({cx, cy}); }

bool ChunkStore::read(int cx, int cy, std::vector<std::uint8_t>& out, const std::vector<MaterialId>** remap) const {
  if (work_.count({cx, cy})) {
    *remap = nullptr;
    return read_file(work_dir_ / file_name(cx, cy), out);
  }
  if (base_.count({cx, cy})) {
    *remap = base_remap_.empty() ? nullptr : &base_remap_;
    return read_file(base_dir_ / file_name(cx, cy), out);
  }
  return false;
}

bool ChunkStore::write(int cx, int cy, const std::vector<std::uint8_t>& bytes) {
  const fs::path path = work_dir_ / file_name(cx, cy);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) return false;
  work_.insert({cx, cy});
  return true;
}

std::vector<std::pair<int, int>> ChunkStore::positions() const {
  std::set<std::pair<int, int>> all = base_;
  all.insert(work_.begin(), work_.end());
  return {all.begin(), all.end()};
}

}  // namespace px
