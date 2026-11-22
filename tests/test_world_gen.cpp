#include <algorithm>
#include <cstdlib>

#include "core/material.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "test_framework.hpp"
#include "world/world_gen.hpp"

namespace {

struct GenFixture {
  px::MaterialRegistry materials;
  px::BaseMaterials ids;
  GenFixture() : ids(px::register_base_materials(materials)) {}
};

bool same_cells(const px::World& a, const px::World& b) {
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x)
      if (a.get(x, y) != b.get(x, y) || a.temperature(x, y) != b.temperature(x, y)) return false;
  return true;
}

}  // namespace

TEST(generation_is_deterministic_per_seed) {
  GenFixture f;
  px::World a(16, 8, f.materials, 1234, px::EdgeX::Wrap);
  px::World b(16, 8, f.materials, 1234, px::EdgeX::Wrap);
  px::World c(16, 8, f.materials, 4321, px::EdgeX::Wrap);
  px::generate_world(a, f.ids);
  px::WorldGenerator(f.ids, 1234, b.width(), b.height()).generate(b, 1);  // single-threaded
  px::generate_world(c, f.ids);
  CHECK(same_cells(a, b));   // thread count does not matter
  CHECK(!same_cells(a, c));  // another seed is another world
}

// Each chunk is a pure function of (seed, x, y): generated alone, in any order,
// it matches the full world. Chunk streaming and save/load rely on this.
TEST(chunks_generate_independently) {
  GenFixture f;
  px::World full(12, 8, f.materials, 99, px::EdgeX::Wrap);
  px::generate_world(full, f.ids);
  const px::WorldGenerator gen(f.ids, 99, full.width(), full.height());
  px::World piecemeal(12, 8, f.materials, 99, px::EdgeX::Wrap);
  for (int cy = piecemeal.height_chunks() - 1; cy >= 0; --cy)
    for (int cx = piecemeal.width_chunks() - 1; cx >= 0; --cx) gen.generate_chunk(piecemeal, cx, cy);
  CHECK(same_cells(full, piecemeal));
  bool matches = true;
  for (int y = 0; y < full.height(); y += 7)
    for (int x = 0; x < full.width(); x += 5) matches = matches && gen.material_at(x, y) == full.get(x, y);
  CHECK(matches);
  CHECK_EQ(gen.material_at(-3, 100), gen.material_at(full.width() - 3, 100));  // wraps
  CHECK_EQ(gen.material_at(5, -1), px::kOutOfBounds);
}

TEST(terrain_joins_up_across_the_seam) {
  GenFixture f;
  const int w = 64 * 64, h = 24 * 64;
  for (std::uint64_t seed : {1ULL, 77ULL, 0xC0FFEEULL}) {
    const px::WorldGenerator gen(f.ids, seed, w, h);
    int steepest = 0;
    for (int x = 1; x < w; ++x) steepest = std::max(steepest, std::abs(gen.surface_y(x) - gen.surface_y(x - 1)));
    const int seam = std::abs(gen.surface_y(0) - gen.surface_y(w - 1));
    CHECK(seam <= std::max(steepest, 1));  // no cliff where the ends meet
    CHECK(gen.biome_at(-1) == gen.biome_at(w - 1));
    // Underground (caves, pools, deposits) is continuous too: the column just
    // past the seam matches the first column.
    int differ = 0;
    for (int y = 0; y < h; ++y) differ += gen.material_at(w, y) != gen.material_at(0, y) ? 1 : 0;
    CHECK_EQ(differ, 0);
  }
}

TEST(generated_world_has_sky_ground_caves_and_pools) {
  GenFixture f;
  px::World w(32, 12, f.materials, 2024, px::EdgeX::Wrap);
  px::generate_world(w, f.ids);
  const std::size_t cells = static_cast<std::size_t>(w.width()) * static_cast<std::size_t>(w.height());
  CHECK_EQ(w.get(10, 0), px::kEmpty);                  // sky at the top
  CHECK_EQ(w.get(10, w.height() - 1), f.ids.stone);    // solid bottom
  CHECK(w.count(px::kEmpty) > cells / 5);              // sky plus caves
  CHECK(w.count(f.ids.stone) > cells / 3);
  CHECK(w.count(f.ids.water) > 1000);
  CHECK(w.count(f.ids.lava) > 1000);
  CHECK(w.count(f.ids.basalt) > 100);
  CHECK(w.count(f.ids.grass) + w.count(f.ids.sand) > static_cast<std::size_t>(w.width()) / 2);  // a topsoil
}

// Lava chambers are sealed in insulating basalt, so they neither leak nor heat
// the rock around them.
TEST(lava_is_sealed_in_basalt) {
  GenFixture f;
  px::World w(32, 12, f.materials, 7, px::EdgeX::Wrap);
  px::generate_world(w, f.ids);
  bool sealed = true;
  for (int y = 0; y < w.height(); ++y)
    for (int x = 0; x < w.width(); ++x) {
      if (w.get(x, y) != f.ids.lava) continue;
      for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
        const px::MaterialId n = w.get(x + dx, y + dy);
        sealed = sealed && (n == f.ids.lava || n == f.ids.basalt);
      }
    }
  CHECK(sealed);
}

// A fresh world is generated at rest: liquids sit in contained pools, so after
// a short settling period almost nothing is left to simulate.
TEST(generated_world_settles_quickly) {
  GenFixture f;
  px::World w(32, 12, f.materials, 31337, px::EdgeX::Wrap);
  px::generate_world(w, f.ids);
  const auto water = w.count(f.ids.water);
  int steps = 0;
  while (steps < 1200 && w.active_chunk_count() > 4) {
    px::step(w);
    ++steps;
  }
  CHECK(steps < 1200);
  CHECK(w.active_chunk_count() <= 4);
  CHECK(water - w.count(f.ids.water) < water / 100);  // no pools boiling away or draining off
}

TEST(insulator_blocks_heat) {
  GenFixture f;
  px::World w(1, 1, f.materials, 3);
  w.fill_rect(0, 20, 64, 30, f.ids.stone);
  w.fill_rect(20, 30, 20, 10, f.ids.basalt);
  w.fill_rect(22, 32, 16, 6, f.ids.lava);
  for (int i = 0; i < 600; ++i) px::step(w);
  CHECK_EQ(w.temperature(30, 35), 1200);       // the lava stays hot...
  CHECK_EQ(w.temperature(30, 31), px::kAmbient);  // ...the basalt stays cool...
  CHECK_EQ(w.temperature(30, 28), px::kAmbient);  // ...and so does the rock outside
  CHECK_EQ(w.active_chunk_count(), 0);
}

// A world three times larger with scale 3 is the same world, three times
// bigger: biomes in the same places, hills three times as tall and wide, and
// caves, pools and deposits three times the size.
TEST(scaled_worlds_are_the_same_world_bigger) {
  GenFixture f;
  const int w = 32 * 64, h = 12 * 64;
  const px::WorldGenerator small(f.ids, 99, w, h);
  px::WorldGenParams big_params;
  big_params.scale = 3.0f;
  const px::WorldGenerator big(f.ids, 99, w * 3, h * 3, big_params);
  int same_biome = 0, close_surface = 0, samples = 0;
  for (int x = 0; x < w; x += 7, ++samples) {
    same_biome += small.biome_at(x) == big.biome_at(x * 3) ? 1 : 0;
    close_surface += std::abs(big.surface_y(x * 3) - small.surface_y(x) * 3) <= 6 ? 1 : 0;
  }
  CHECK(same_biome * 100 >= samples * 97);
  CHECK(close_surface * 100 >= samples * 95);
  // Underground, cell (x, y) of the small world matches cell (3x+1, 3y+1) of the big one.
  int same_cell = 0, cells = 0;
  for (int y = h / 3; y < h; y += 5)
    for (int x = 0; x < w; x += 11, ++cells) same_cell += small.material_at(x, y) == big.material_at(x * 3 + 1, y * 3 + 1) ? 1 : 0;
  CHECK(same_cell * 100 >= cells * 85);
}
