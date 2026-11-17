#include <algorithm>
#include <vector>

#include "core/material.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "test_framework.hpp"

namespace {

struct Fixture {
  px::MaterialRegistry materials;
  px::BaseMaterials ids;
  Fixture() : ids(px::register_base_materials(materials)) {}
};

void run(px::World& w, int steps) {
  for (int i = 0; i < steps; ++i) px::step(w);
}

/// Runs until no chunk is active, up to `max_steps`. Returns steps taken.
int run_until_settled(px::World& w, int max_steps) {
  for (int i = 0; i < max_steps; ++i) {
    px::step(w);
    if (w.active_chunk_count() == 0) return i + 1;
  }
  return max_steps;
}

}  // namespace

TEST(registry_has_empty_at_zero_and_rejects_duplicates) {
  Fixture f;
  CHECK_EQ(f.materials.def(px::kEmpty).name, std::string("empty"));
  CHECK(f.materials.find("sand") == f.ids.sand);
  bool threw = false;
  try {
    px::MaterialDef dup;
    dup.name = "sand";
    f.materials.add(dup);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(out_of_bounds_reads_are_walls_and_writes_are_ignored) {
  Fixture f;
  px::World w(1, 1, f.materials, 1);
  CHECK_EQ(w.get(-1, 0), px::kOutOfBounds);
  CHECK_EQ(w.get(0, 64), px::kOutOfBounds);
  w.set(-5, -5, f.ids.sand);  // must not crash
  CHECK_EQ(w.count(f.ids.sand), std::size_t{0});
}

TEST(sand_falls_to_the_floor) {
  Fixture f;
  px::World w(1, 1, f.materials, 1);
  w.set(10, 0, f.ids.sand);
  run(w, 200);
  CHECK_EQ(w.get(10, 63), f.ids.sand);
  CHECK_EQ(w.count(f.ids.sand), std::size_t{1});
}

TEST(sand_falls_across_chunk_boundaries) {
  Fixture f;
  px::World w(2, 3, f.materials, 1);
  w.set(70, 5, f.ids.sand);  // chunk (1, 0)
  run(w, 400);
  CHECK_EQ(w.get(70, w.height() - 1), f.ids.sand);  // chunk (1, 2)
}

TEST(stone_never_moves) {
  Fixture f;
  px::World w(1, 1, f.materials, 1);
  w.fill_rect(20, 10, 5, 3, f.ids.stone);
  run(w, 100);
  for (int y = 10; y < 13; ++y)
    for (int x = 20; x < 25; ++x) CHECK_EQ(w.get(x, y), f.ids.stone);
}

TEST(sand_column_collapses_into_a_stable_pile) {
  Fixture f;
  px::World w(2, 2, f.materials, 7);
  w.fill_rect(60, 0, 4, 60, f.ids.sand);
  run_until_settled(w, 3000);

  // Every grain rests on something and has no free diagonal below it.
  int max_height = 0;
  for (int x = 0; x < w.width(); ++x) {
    int col = 0;
    for (int y = 0; y < w.height(); ++y) {
      if (w.get(x, y) != f.ids.sand) continue;
      ++col;
      CHECK(w.get(x, y + 1) != px::kEmpty);
      CHECK(w.get(x - 1, y + 1) != px::kEmpty);
      CHECK(w.get(x + 1, y + 1) != px::kEmpty);
    }
    max_height = std::max(max_height, col);
  }
  CHECK(max_height < 60);  // it spread out
  CHECK_EQ(w.count(f.ids.sand), std::size_t{240});
}

TEST(settled_world_goes_to_sleep) {
  Fixture f;
  px::World w(3, 2, f.materials, 3);
  w.fill_rect(30, 5, 40, 20, f.ids.sand);
  const int steps = run_until_settled(w, 5000);
  CHECK(steps < 5000);
  CHECK_EQ(w.active_chunk_count(), 0);
  // A sleeping world costs nothing to step.
  const px::StepStats st = px::step(w);
  CHECK_EQ(st.cells_scanned, std::int64_t{0});
}

TEST(sand_sinks_below_water) {
  Fixture f;
  px::World w(1, 1, f.materials, 5);
  // A narrow well: water at the bottom, sand on top.
  w.fill_rect(10, 0, 1, 64, f.ids.stone);
  w.fill_rect(15, 0, 1, 64, f.ids.stone);
  w.fill_rect(11, 44, 4, 20, f.ids.water);
  w.fill_rect(11, 34, 4, 10, f.ids.sand);
  run(w, 1500);
  for (int y = 54; y < 64; ++y)
    for (int x = 11; x < 15; ++x) CHECK_EQ(w.get(x, y), f.ids.sand);
  for (int y = 34; y < 54; ++y)
    for (int x = 11; x < 15; ++x) CHECK_EQ(w.get(x, y), f.ids.water);
}

TEST(water_levels_out_in_a_basin) {
  Fixture f;
  px::World w(2, 1, f.materials, 9);
  w.fill_rect(0, 0, 40, 64, f.ids.water);  // a tall block against the left wall
  run(w, 3000);
  int min_col = 64, max_col = 0;
  for (int x = 0; x < w.width(); ++x) {
    int col = 0;
    for (int y = 0; y < w.height(); ++y) col += w.get(x, y) == f.ids.water ? 1 : 0;
    min_col = std::min(min_col, col);
    max_col = std::max(max_col, col);
  }
  CHECK(max_col - min_col <= 1);
  CHECK_EQ(w.count(f.ids.water), std::size_t{40 * 64});
}

TEST(mass_is_conserved) {
  Fixture f;
  px::World w(4, 3, f.materials, 11);
  w.fill_rect(0, 150, 256, 42, f.ids.stone);
  for (int i = 0; i < 30; ++i) {
    w.fill_circle(10 + i * 8, 20 + (i * 37) % 80, 6, (i % 2) ? f.ids.sand : f.ids.water, 0.6f);
  }
  const auto sand = w.count(f.ids.sand);
  const auto water = w.count(f.ids.water);
  const auto stone = w.count(f.ids.stone);
  run(w, 800);
  CHECK_EQ(w.count(f.ids.sand), sand);
  CHECK_EQ(w.count(f.ids.water), water);
  CHECK_EQ(w.count(f.ids.stone), stone);
}

TEST(same_seed_and_input_gives_identical_worlds) {
  Fixture f;
  auto build = [&](px::World& w) {
    w.fill_rect(0, 100, 128, 28, f.ids.stone);
    w.fill_rect(20, 10, 30, 30, f.ids.sand);
    w.fill_rect(70, 10, 30, 30, f.ids.water);
  };
  px::World a(2, 2, f.materials, 42);
  px::World b(2, 2, f.materials, 42);
  build(a);
  build(b);
  run(a, 500);
  run(b, 500);
  bool identical = true;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) identical = identical && a.get(x, y) == b.get(x, y);
  CHECK(identical);
}

TEST(only_dirty_regions_are_scanned) {
  Fixture f;
  px::World w(8, 8, f.materials, 2);  // 262k cells
  w.set(5, 5, f.ids.sand);
  px::step(w);  // consumes the initial wake
  const px::StepStats st = px::step(w);
  CHECK(st.active_chunks <= 2);
  CHECK(st.cells_scanned < 64);
}
