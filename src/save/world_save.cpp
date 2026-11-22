#include "save/world_save.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#include "save/bytes.hpp"
#include "save/chunk_codec.hpp"
#include "save/chunk_store.hpp"

namespace px {
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kWorldMagic = 0x44575850;  // "PXWD"
// 2: player position; 3: generator scale. Older saves still load.
constexpr std::uint16_t kWorldVersion = 3;
constexpr const char* kHeaderName = "world.pxw";
constexpr const char* kChunkDirName = "chunks";

bool fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  return static_cast<bool>(out);
}

std::vector<std::uint8_t> encode_header(const World& world, const SaveInfo& info) {
  ByteWriter w;
  w.u32(kWorldMagic);
  w.u16(kWorldVersion);
  w.i32(world.width_chunks());
  w.i32(world.height_chunks());
  w.u8(world.wraps_x() ? 1 : 0);
  w.u64(world.seed());
  w.u32(world.tick());
  w.i32(info.view_x);
  w.i32(info.view_y);
  w.u8(info.has_player ? 1 : 0);
  w.f32(info.player_x);
  w.f32(info.player_y);
  w.u8(info.generator ? 1 : 0);
  if (info.generator) {
    const WorldGenParams& g = *info.generator;
    w.f32(g.surface);
    w.f32(g.sea_level_offset);
    w.f32(g.cave_start);
    w.f32(g.lava_depth);
    w.f32(g.oil_depth);
    w.i32(g.bedrock_rows);
    w.f32(g.scale);
  }
  // Material table, in id order: authored by name, derived by recipe.
  const MaterialRegistry& reg = world.materials();
  w.varint(static_cast<std::uint32_t>(reg.size()));
  for (std::size_t id = 0; id < reg.size(); ++id) {
    const auto rec = reg.variant_record(static_cast<MaterialId>(id));
    if (!rec) {
      w.u8(0);
      w.str(reg.def(static_cast<MaterialId>(id)).name);
      continue;
    }
    w.u8(1);
    w.str(reg.def(rec->base).name);
    w.i32(rec->density);
    w.i32(rec->conductivity);
    w.i32(rec->ignition);
    w.i32(rec->flammability);
    w.u8(rec->r);
    w.u8(rec->g);
    w.u8(rec->b);
  }
  std::vector<std::uint8_t>& out = w.bytes();
  const std::uint32_t sum = checksum(out.data(), out.size());
  w.u32(sum);
  return std::move(w.bytes());
}

}  // namespace

bool save_world(const World& world, const SaveInfo& info, const fs::path& dir, const ChunkStore* store,
                std::string* error) {
  std::error_code ec;
  const fs::path tmp = fs::path(dir.string() + ".saving");
  fs::remove_all(tmp, ec);
  if (!fs::create_directories(tmp / kChunkDirName, ec)) return fail(error, "cannot create " + tmp.string());

  auto put_chunk = [&](int cx, int cy, const std::vector<std::uint8_t>& bytes) {
    return write_file(tmp / kChunkDirName / ChunkStore::file_name(cx, cy), bytes);
  };
  bool ok = true;
  std::string why;
  for (int cy = 0; cy < world.height_chunks() && ok; ++cy) {
    for (int cx = 0; cx < world.width_chunks() && ok; ++cx) {
      if (const Chunk* c = world.chunk_ptr(cx, cy)) {
        // In memory: written if it differs from generation, now or since it was loaded.
        if (c->touched || (store && store->contains(cx, cy))) {
          ok = put_chunk(cx, cy, encode_chunk(*c, cx, cy, world.seed()));
        }
        continue;
      }
      if (!store || !store->contains(cx, cy)) continue;  // untouched: regenerated on load
      std::vector<std::uint8_t> bytes;
      const std::vector<MaterialId>* remap = nullptr;
      if (!store->read(cx, cy, bytes, &remap)) {
        ok = false;
        why = "cannot read stored chunk " + ChunkStore::file_name(cx, cy);
        break;
      }
      if (remap) {
        // From the loaded save, with its own material ids: re-encode with ours.
        Chunk chunk;
        if (!decode_chunk(bytes.data(), bytes.size(), cx, cy, world.seed(), remap, world.materials().size(), chunk,
                          &why)) {
          ok = false;
          break;
        }
        bytes = encode_chunk(chunk, cx, cy, world.seed());
      }
      ok = put_chunk(cx, cy, bytes);
    }
  }
  if (ok) ok = write_file(tmp / kHeaderName, encode_header(world, info));
  if (!ok) {
    fs::remove_all(tmp, ec);
    return fail(error, why.empty() ? "cannot write " + tmp.string() : why);
  }
  // Swap the finished save in.
  const fs::path old = fs::path(dir.string() + ".old");
  fs::remove_all(old, ec);
  if (fs::exists(dir)) {
    fs::rename(dir, old, ec);
    if (ec) {
      fs::remove_all(tmp, ec);
      return fail(error, "cannot replace " + dir.string());
    }
  }
  fs::rename(tmp, dir, ec);
  if (ec) {
    if (fs::exists(old)) fs::rename(old, dir, ec);  // put the previous save back
    return fail(error, "cannot move the save into " + dir.string());
  }
  fs::remove_all(old, ec);
  return true;
}

void mark_saved(World& world, ChunkStore& store, const fs::path& dir) {
  store.rebase(dir / kChunkDirName);
  for (int cy = 0; cy < world.height_chunks(); ++cy)
    for (int cx = 0; cx < world.width_chunks(); ++cx)
      if (Chunk* c = world.chunk_ptr(cx, cy)) c->touched = false;
}

std::optional<OpenedSave> open_save(const fs::path& dir, MaterialRegistry& materials, std::string* error) {
  std::ifstream in(dir / kHeaderName, std::ios::binary);
  if (!in) {
    fail(error, "no save in " + dir.string());
    return std::nullopt;
  }
  const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() < 8) {
    fail(error, "save header too short");
    return std::nullopt;
  }
  std::uint32_t stored = 0;
  for (int k = 0; k < 4; ++k) stored |= static_cast<std::uint32_t>(data[data.size() - 4 + static_cast<std::size_t>(k)]) << (8 * k);
  if (checksum(data.data(), data.size() - 4) != stored) {
    fail(error, "save header damaged (checksum mismatch)");
    return std::nullopt;
  }
  ByteReader r(data.data(), data.size() - 4);
  if (r.u32() != kWorldMagic) {
    fail(error, "not a save header");
    return std::nullopt;
  }
  const std::uint16_t version = r.u16();
  if (version < 1 || version > kWorldVersion) {
    fail(error, "unsupported save format version");
    return std::nullopt;
  }
  const int wc = r.i32(), hc = r.i32();
  const bool wrap = r.u8() != 0;
  const std::uint64_t seed = r.u64();
  const std::uint32_t tick = r.u32();
  OpenedSave save;
  save.info.view_x = r.i32();
  save.info.view_y = r.i32();
  if (version >= 2) {
    save.info.has_player = r.u8() != 0;
    save.info.player_x = r.f32();
    save.info.player_y = r.f32();
  }
  if (r.u8()) {
    WorldGenParams g;
    g.surface = r.f32();
    g.sea_level_offset = r.f32();
    g.cave_start = r.f32();
    g.lava_depth = r.f32();
    g.oil_depth = r.f32();
    g.bedrock_rows = r.i32();
    if (version >= 3) g.scale = r.f32();  // earlier saves: scale 1
    save.info.generator = g;
  }
  if (!r.ok() || wc <= 0 || hc <= 0 || wc > 1 << 16 || hc > 1 << 16) {
    fail(error, "save header damaged");
    return std::nullopt;
  }
  const std::uint32_t count = r.varint();
  if (!r.ok() || count == 0 || count > kMaxMaterials) {
    fail(error, "save material table damaged");
    return std::nullopt;
  }
  save.remap.resize(count, kEmpty);
  for (std::uint32_t id = 0; id < count; ++id) {
    const std::uint8_t kind = r.u8();
    const std::string name = r.str();
    const auto found = materials.find(name);
    if (!r.ok()) break;
    if (!found || *found >= materials.authored_count()) {
      fail(error, "save uses material '" + name + "', which this version does not have");
      return std::nullopt;
    }
    if (kind == 0) {
      save.remap[id] = *found;
      continue;
    }
    VariantRecord rec;
    rec.base = *found;
    rec.density = r.i32();
    rec.conductivity = r.i32();
    rec.ignition = r.i32();
    rec.flammability = r.i32();
    rec.r = r.u8();
    rec.g = r.u8();
    rec.b = r.u8();
    save.remap[id] = materials.variant(rec);
  }
  if (!r.ok() || r.remaining() != 0) {
    fail(error, "save header damaged");
    return std::nullopt;
  }
  save.world = std::make_unique<World>(wc, hc, materials, seed, wrap ? EdgeX::Wrap : EdgeX::Wall, ChunkStorage::Streamed);
  save.world->set_tick(tick);
  save.chunk_dir = dir / kChunkDirName;
  return save;
}

std::unique_ptr<World> load_world(const fs::path& dir, MaterialRegistry& materials, const BaseMaterials& ids,
                                  std::string* error) {
  std::optional<OpenedSave> save = open_save(dir, materials, error);
  if (!save) return nullptr;
  World& w = *save->world;
  std::optional<WorldGenerator> gen;
  if (save->info.generator) gen.emplace(ids, w.seed(), w.width(), w.height(), *save->info.generator);
  for (int cy = 0; cy < w.height_chunks(); ++cy) {
    for (int cx = 0; cx < w.width_chunks(); ++cx) {
      std::ifstream in(save->chunk_dir / ChunkStore::file_name(cx, cy), std::ios::binary);
      if (in) {
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto chunk = std::make_unique<Chunk>();
        if (!decode_chunk(bytes.data(), bytes.size(), cx, cy, w.seed(), &save->remap, materials.size(), *chunk, error)) {
          return nullptr;
        }
        w.load_chunk(cx, cy, std::move(chunk));
      } else if (gen) {
        gen->generate_chunk(w, cx, cy);
      } else {
        w.load_chunk(cx, cy, w.make_empty_chunk(cy));
      }
    }
  }
  return std::move(save->world);
}

}  // namespace px
