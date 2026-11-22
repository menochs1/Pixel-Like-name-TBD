// Saving, loading and streaming chunks.

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/rng.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "save/chunk_codec.hpp"
#include "save/chunk_store.hpp"
#include "save/streamer.hpp"
#include "save/world_save.hpp"
#include "test_framework.hpp"
#include "world/demo_scene.hpp"
#include "world/world_gen.hpp"

namespace fs = std::filesystem;

namespace {

/// A fresh directory under the system temp dir, removed afterwards.
struct TempDir {
  fs::path path;
  TempDir() {
    static std::atomic<int> counter{0};
    path = fs::temp_directory_path() /
           ("px_test_" + std::to_string(px::hash32(static_cast<std::uint32_t>(std::rand()))) + "_" +
            std::to_string(counter++));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct SaveFixture {
  px::MaterialRegistry materials;
  px::BaseMaterials ids;
  SaveFixture() : ids(px::register_base_materials(materials)) {}
};

bool same_cells(const px::World& a, const px::World& b) {
  if (a.width() != b.width() || a.height() != b.height()) return false;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x)
      if (a.get(x, y) != b.get(x, y) || a.temperature(x, y) != b.temperature(x, y) || a.health(x, y) != b.health(x, y))
        return false;
  return true;
}

/// Same material by meaning (element name and properties), for worlds whose
/// registries number materials differently.
bool same_meaning(const px::World& a, const px::World& b) {
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) {
      const px::MaterialRegistry& ra = a.materials();
      const px::MaterialRegistry& rb = b.materials();
      const px::MaterialId ma = a.get(x, y), mb = b.get(x, y);
      if (ra.def(ra.props(ma).base).name != rb.def(rb.props(mb).base).name) return false;
      if (ra.props(ma).density != rb.props(mb).density) return false;
      if (a.temperature(x, y) != b.temperature(x, y)) return false;
    }
  return true;
}

int count_files(const fs::path& dir) {
  int n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) n += e.is_regular_file() ? 1 : 0;
  return n;
}

}  // namespace

TEST(chunk_codec_round_trips_every_field) {
  SaveFixture f;
  px::Chunk c;
  std::uint32_t r = 12345;
  auto rnd = [&](std::uint32_t n) {
    r = px::hash32(r + 0x9E3779B9U);
    return r % n;
  };
  for (int i = 0; i < px::kChunkArea; ++i) {
    const std::size_t k = static_cast<std::size_t>(i);
    c.material[k] = static_cast<px::MaterialId>(i < 2000 ? f.ids.stone : rnd(static_cast<std::uint32_t>(f.materials.size())));
    c.thermal[k] = static_cast<std::uint16_t>(rnd(65536));
    c.motion[k] = static_cast<std::uint16_t>(rnd(4) == 0 ? rnd(65536) : 0);
    c.health[k] = static_cast<std::uint8_t>(rnd(3) == 0 ? rnd(256) : 255);
  }
  c.next_dirty.include(3, 9);
  c.next_dirty.include(40, 60);
  const auto bytes = px::encode_chunk(c, 7, -2, 99);
  px::Chunk d;
  std::string error;
  CHECK(px::decode_chunk(bytes.data(), bytes.size(), 7, -2, 99, nullptr, f.materials.size(), d, &error));
  CHECK(d.material == c.material);
  CHECK(d.thermal == c.thermal);
  CHECK(d.motion == c.motion);
  CHECK(d.health == c.health);
  CHECK_EQ(d.next_dirty.min_x, 3);
  CHECK_EQ(d.next_dirty.max_y, 60);
  CHECK(!d.touched);

  // Damage is detected, never loaded as garbage.
  auto broken = bytes;
  broken[broken.size() / 2] ^= 0x10;
  CHECK(!px::decode_chunk(broken.data(), broken.size(), 7, -2, 99, nullptr, f.materials.size(), d, &error));
  CHECK(!px::decode_chunk(bytes.data(), bytes.size() - 9, 7, -2, 99, nullptr, f.materials.size(), d, &error));
  CHECK(!px::decode_chunk(bytes.data(), bytes.size(), 8, -2, 99, nullptr, f.materials.size(), d, &error));
  CHECK(!px::decode_chunk(bytes.data(), bytes.size(), 7, -2, 99, nullptr, 3, d, &error));  // unknown ids
}

TEST(chunk_codec_is_compact_for_terrain) {
  SaveFixture f;
  px::World w(2, 2, f.materials, 4);
  w.fill_rect(0, 64, 128, 64, f.ids.stone);  // shades come from World::set: position-derived
  const auto rock = px::encode_chunk(w.chunk(0, 1), 0, 1, w.seed());
  const auto air = px::encode_chunk(w.chunk(0, 0), 0, 0, w.seed());
  CHECK(rock.size() < 64);
  CHECK(air.size() < 64);
}

TEST(save_and_load_restore_a_world_that_then_runs_identically) {
  SaveFixture f;
  TempDir tmp;
  px::World w(8, 5, f.materials, 0xC0FFEE);
  px::build_demo_scene(w, f.ids);
  w.fill_circle(256, 230, 5, f.ids.fire);       // light the oil pit
  w.fill_rect(300, 40, 20, 6, f.ids.sand);       // and make some tar
  w.fill_rect(300, 34, 20, 6, f.ids.oil);
  w.damage(100, w.height() - 5, 50);
  for (int i = 0; i < 300; ++i) px::step(w);
  std::string error;
  CHECK(px::save_world(w, {}, tmp.path / "save", nullptr, &error));
  std::unique_ptr<px::World> loaded = px::load_world(tmp.path / "save", f.materials, f.ids, &error);
  CHECK(loaded != nullptr);
  if (!loaded) return;
  CHECK(same_cells(w, *loaded));
  CHECK_EQ(loaded->tick(), w.tick());
  CHECK_EQ(loaded->seed(), w.seed());
  // Pending work, motion and lifetimes came back too: both keep going in lockstep.
  for (int i = 0; i < 400; ++i) {
    px::step(w);
    px::step(*loaded);
  }
  CHECK(same_cells(w, *loaded));
}

// Materials are saved by name and derived ones by recipe, so a save loads
// into a registry that numbers them differently.
TEST(saves_survive_material_ids_changing) {
  SaveFixture f;
  TempDir tmp;
  px::World w(1, 1, f.materials, 3);
  w.fill_rect(0, 56, 64, 8, f.ids.stone);
  w.fill_rect(0, 50, 64, 6, f.ids.sand);
  w.fill_rect(0, 44, 64, 6, f.ids.oil);
  for (int i = 0; i < 300; ++i) px::step(w);
  CHECK(f.materials.derived_count() > 0);  // tar variants exist
  CHECK(px::save_world(w, {}, tmp.path / "s", nullptr));

  px::MaterialRegistry other;
  px::MaterialDef extra;
  extra.name = "glitter";  // shifts every built-in id by one
  extra.phase = px::Phase::Powder;
  other.add(extra);
  const px::BaseMaterials other_ids = px::register_base_materials(other);
  std::string error;
  std::unique_ptr<px::World> loaded = px::load_world(tmp.path / "s", other, other_ids, &error);
  CHECK(loaded != nullptr);
  if (!loaded) return;
  CHECK(other_ids.stone != f.ids.stone);
  CHECK(same_meaning(w, *loaded));
  CHECK(other.derived_count() > 0);  // the tar variants were recreated

  px::MaterialRegistry bare;  // knows none of the built-in materials
  CHECK(!px::open_save(tmp.path / "s", bare, &error));
  CHECK(error.find("which this version does not have") != std::string::npos);
  CHECK(!px::open_save(tmp.path / "nowhere", bare, &error));
}

TEST(generated_worlds_save_only_what_changed) {
  SaveFixture f;
  TempDir tmp;
  px::World w(16, 8, f.materials, 77, px::EdgeX::Wrap);
  px::generate_world(w, f.ids);
  for (int i = 0; i < 120; ++i) px::step(w);
  w.fill_circle(500, 100, 6, f.ids.water);  // one change in one place
  px::SaveInfo info;
  info.generator = px::WorldGenParams{};
  CHECK(px::save_world(w, info, tmp.path / "g", nullptr));
  const int written = count_files(tmp.path / "g" / "chunks");
  CHECK(written > 0);
  CHECK(written < 16 * 8 / 2);  // the rest regenerate from the seed
  std::string error;
  std::unique_ptr<px::World> loaded = px::load_world(tmp.path / "g", f.materials, f.ids, &error);
  CHECK(loaded != nullptr);
  if (loaded) CHECK(same_cells(w, *loaded));
}

// The central guarantee: a world streamed through disk, with chunks unloaded
// and reloaded as the camera moves (across the wrap seam, with fires and
// water painted on the way and a save in the middle), evolves exactly like
// the same world held fully in memory.
TEST(streamed_world_matches_a_world_held_in_memory) {
  SaveFixture f;
  TempDir tmp;
  const int wc = 16, hc = 6;
  px::World full(wc, hc, f.materials, 2024, px::EdgeX::Wrap);
  px::generate_world(full, f.ids);
  const px::WorldGenerator gen(f.ids, 2024, full.width(), full.height());

  px::World streamed(wc, hc, f.materials, 2024, px::EdgeX::Wrap, px::ChunkStorage::Streamed);
  px::ChunkStore store(tmp.path / "work");
  px::ChunkStreamer streamer(streamed, store, &gen);

  std::vector<px::MaterialId> snapshot;  // the full world at the moment of the save
  std::vector<px::Temperature> snapshot_t;
  int most_loaded = 0;
  bool consistent = true;
  for (int s = 1; s <= 900; ++s) {
    const int cam = (s / 12) - 3;  // the camera drifts right, wrapping round once
    const px::ChunkRegion region{cam, 0, cam + 2, hc - 1};
    const px::ChunkRegion required{cam - 1, 0, cam + 3, hc - 1};
    const px::ChunkRegion wanted{cam - 2, 0, cam + 4, hc - 1};
    const px::ChunkRegion keep{cam - 3, 0, cam + 5, hc - 1};
    streamer.update(required, wanted, keep, 3);
    most_loaded = std::max(most_loaded, streamed.loaded_chunk_count());
    if (s % 40 == 1) {  // paint inside the simulated region, in both worlds
      const int x = (cam + 1) * px::kChunkSize + 20, y = 40 + (s * 7) % 200;
      const px::MaterialId m = (s / 40) % 3 == 0 ? f.ids.fire : ((s / 40) % 3 == 1 ? f.ids.water : f.ids.sand);
      full.fill_circle(x, y, 6, m, 0.7f);
      streamed.fill_circle(x, y, 6, m, 0.7f);
    }
    if (s == 450) {
      px::SaveInfo info;
      info.generator = px::WorldGenParams{};
      std::string error;
      consistent = consistent && px::save_world(streamed, info, tmp.path / "mid", &store, &error);
      px::mark_saved(streamed, store, tmp.path / "mid");
      for (int y = 0; y < full.height(); ++y)
        for (int x = 0; x < full.width(); ++x) {
          snapshot.push_back(full.get(x, y));
          snapshot_t.push_back(full.temperature(x, y));
        }
    }
    px::step(full, region);
    px::step(streamed, region);
  }
  CHECK(consistent);
  CHECK(most_loaded < wc * hc);   // it really streamed
  CHECK(most_loaded <= 9 * hc);   // never more than the keep box
  // Bring everything back and compare with the world that never left memory.
  streamer.update({0, 0, wc - 1, hc - 1}, {0, 0, -1, -1}, {0, 0, wc - 1, hc - 1}, 0);
  CHECK_EQ(streamed.loaded_chunk_count(), wc * hc);
  CHECK(same_cells(full, streamed));

  // The save from the middle loads (through a store base, with its remap) as
  // the full world was at that moment.
  std::string error;
  std::optional<px::OpenedSave> save = px::open_save(tmp.path / "mid", f.materials, &error);
  CHECK(save.has_value());
  if (!save) return;
  px::ChunkStore store2(tmp.path / "work2");
  store2.set_base(save->chunk_dir, save->remap);
  px::ChunkStreamer streamer2(*save->world, store2, &gen);
  streamer2.update({0, 0, wc - 1, hc - 1}, {0, 0, -1, -1}, {0, 0, wc - 1, hc - 1}, 0);
  bool match = true;
  std::size_t i = 0;
  for (int y = 0; y < full.height(); ++y)
    for (int x = 0; x < full.width(); ++x, ++i)
      match = match && save->world->get(x, y) == snapshot[i] && save->world->temperature(x, y) == snapshot_t[i];
  CHECK(match);
}

TEST(streamer_regenerates_a_damaged_chunk_and_reports_it) {
  SaveFixture f;
  TempDir tmp;
  px::World w(4, 2, f.materials, 5, px::EdgeX::Wrap, px::ChunkStorage::Streamed);
  const px::WorldGenerator gen(f.ids, 5, w.width(), w.height());
  px::ChunkStore store(tmp.path / "work");
  px::ChunkStreamer streamer(w, store, &gen);
  CHECK(streamer.load(1, 1));
  w.fill_circle(64 + 10, 64 + 10, 5, f.ids.water);  // touch it
  CHECK(streamer.unload(1, 1));                      // written
  CHECK(store.contains(1, 1));
  {
    std::ofstream out(store.work_dir() / px::ChunkStore::file_name(1, 1), std::ios::binary | std::ios::trunc);
    out << "garbage";
  }
  CHECK(!streamer.load(1, 1));  // could not read it...
  CHECK(w.loaded(1, 1));        // ...so it was regenerated rather than left as a hole
  CHECK(!streamer.last_error().empty());
}

TEST(saves_remember_the_player_and_the_view) {
  SaveFixture f;
  TempDir tmp;
  px::World w(2, 2, f.materials, 6);
  px::SaveInfo info;
  info.view_x = -123;
  info.view_y = 45;
  info.has_player = true;
  info.player_x = 1234.75f;
  info.player_y = 56.5f;
  CHECK(px::save_world(w, info, tmp.path / "p", nullptr));
  std::optional<px::OpenedSave> save = px::open_save(tmp.path / "p", f.materials);
  CHECK(save.has_value());
  if (!save) return;
  CHECK_EQ(save->info.view_x, -123);
  CHECK_EQ(save->info.view_y, 45);
  CHECK(save->info.has_player);
  CHECK(save->info.player_x == 1234.75f);
  CHECK(save->info.player_y == 56.5f);
}
