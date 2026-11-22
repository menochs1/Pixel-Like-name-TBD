#include <algorithm>
#include <cstdlib>
#include <stdexcept>
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

// A basin whose water does not fill a whole number of rows leaves a partial
// top layer. Those surface cells have nowhere lower to go, so they must stop
// shuffling sideways and let the basin sleep.
TEST(partial_liquid_surface_goes_to_sleep) {
  Fixture f;
  px::World w(2, 1, f.materials, 17);
  w.fill_rect(0, 0, 128, 1, f.ids.stone);  // lid: nothing falls in from outside
  w.fill_rect(0, 30, 70, 34, f.ids.water);  // 2380 cells: 18 full rows + 76 over a 128-wide floor
  const int steps = run_until_settled(w, 6000);
  CHECK(steps < 6000);
  CHECK_EQ(w.active_chunk_count(), 0);
  CHECK_EQ(w.count(f.ids.water), std::size_t{70 * 34});
  // Settling must not leave holes: every row below the surface layer is full.
  for (int y = w.height() - 18; y < w.height(); ++y)
    for (int x = 0; x < w.width(); ++x) CHECK_EQ(w.get(x, y), f.ids.water);
}

// A settled liquid must still react when its container opens up.
TEST(settled_liquid_pours_when_its_wall_is_removed) {
  Fixture f;
  px::World w(2, 1, f.materials, 23);
  w.fill_rect(40, 0, 2, 64, f.ids.stone);   // wall
  w.fill_rect(0, 20, 40, 44, f.ids.water);  // pool left of the wall, with a partial top row
  w.set(0, 19, f.ids.water);
  CHECK(run_until_settled(w, 6000) < 6000);
  w.fill_rect(40, 0, 2, 64, px::kEmpty);
  run(w, 3000);
  int right_side = 0;
  for (int y = 0; y < w.height(); ++y)
    for (int x = 64; x < w.width(); ++x) right_side += w.get(x, y) == f.ids.water ? 1 : 0;
  CHECK(right_side > 500);  // a good share flowed across
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

// Chunks and dirty rects are only an optimisation: skipping sleeping cells must
// give exactly the same world as updating every cell every step. A mismatch here
// shows up in play as liquids terracing along chunk edges.
TEST(chunk_sleeping_matches_updating_everything) {
  Fixture f;
  px::World chunked(6, 3, f.materials, 0xC0FFEE);
  px::World full(6, 3, f.materials, 0xC0FFEE);
  for (px::World* w : {&chunked, &full}) {
    w->fill_rect(0, w->height() - 8, w->width(), 8, f.ids.stone);
    w->fill_rect(150, 120, 30, 6, f.ids.stone);
    w->fill_rect(10, 40, 100, 140, f.ids.water);
    w->fill_rect(200, 10, 40, 60, f.ids.sand);
  }
  bool identical = true;
  for (int s = 0; s < 400 && identical; ++s) {
    px::step(chunked);
    full.wake_all();
    px::step(full);
    for (int y = 0; y < full.height() && identical; ++y)
      for (int x = 0; x < full.width(); ++x)
        if (chunked.get(x, y) != full.get(x, y)) {
          identical = false;
          break;
        }
  }
  CHECK(identical);
}

TEST(water_crosses_chunk_edges_without_terracing) {
  Fixture f;
  px::World w(6, 3, f.materials, 0xC0FFEE);
  w.fill_rect(0, w.height() - 8, w.width(), 8, f.ids.stone);
  w.fill_rect(10, 40, 100, 140, f.ids.water);
  run(w, 400);  // the initial surge sloshes for a few seconds, then settles
  int min_top = w.height(), max_top = 0;
  for (int x = 0; x < w.width(); ++x) {
    int top = w.height();
    for (int y = 0; y < w.height(); ++y)
      if (w.get(x, y) == f.ids.water) {
        top = y;
        break;
      }
    min_top = std::min(min_top, top);
    max_top = std::max(max_top, top);
  }
  CHECK(max_top - min_top <= 2);  // one flat sheet across all six chunks
  // ...that then goes to sleep instead of shuffling forever.
  CHECK(run_until_settled(w, 3000) < 3000);
}

// ------------------------------------------------------------------ step 2:
// fire, smoke, oil, temperature, reactions

namespace {

int count_in(const px::World& w, px::MaterialId m) { return static_cast<int>(w.count(m)); }

/// A closed stone box: walls on all sides, interior [x0+1, x1-1] x [y0+1, y1-1].
void stone_box(px::World& w, px::MaterialId stone, int x0, int y0, int x1, int y1) {
  w.fill_rect(x0, y0, x1 - x0 + 1, 1, stone);
  w.fill_rect(x0, y1, x1 - x0 + 1, 1, stone);
  w.fill_rect(x0, y0, 1, y1 - y0 + 1, stone);
  w.fill_rect(x1, y0, 1, y1 - y0 + 1, stone);
}

}  // namespace

TEST(registry_resolves_reactions_in_both_orders) {
  Fixture f;
  const px::Reaction* r = f.materials.reaction(f.ids.lava, f.ids.water);
  CHECK(r != nullptr);
  if (r) {
    CHECK_EQ(r->a_to, f.ids.stone);
    CHECK_EQ(r->b_to, f.ids.steam);
  }
  const px::Reaction* back = f.materials.reaction(f.ids.water, f.ids.lava);
  CHECK(back != nullptr);
  if (back) {
    CHECK_EQ(back->a_to, f.ids.steam);
    CHECK_EQ(back->b_to, f.ids.stone);
  }
  CHECK(f.materials.reaction(f.ids.sand, f.ids.water) == nullptr);
  // Only the first-named side scans for partners, keeping water on the fast path.
  CHECK(f.materials.props(f.ids.lava).has_reactions);
  CHECK(!f.materials.props(f.ids.water).active_at_ambient);
  CHECK(!f.materials.props(f.ids.sand).active_at_ambient);
}

TEST(registry_rejects_unknown_material_names) {
  px::MaterialRegistry reg;
  px::MaterialDef d;
  d.name = "goo";
  d.phase = px::Phase::Liquid;
  d.high_temp = 50;
  d.high_to = "no_such_material";
  reg.add(d);
  bool threw = false;
  try {
    reg.finalize();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(oil_floats_on_water) {
  Fixture f;
  px::World w(1, 1, f.materials, 3);
  w.fill_rect(20, 30, 10, 20, f.ids.oil);    // oil underneath
  w.fill_rect(20, 10, 10, 20, f.ids.water);  // water on top
  stone_box(w, f.ids.stone, 19, 5, 30, 63);
  run(w, 1500);
  // Inside the box: oil occupies the top band, water the bottom band.
  for (int x = 20; x < 30; ++x) {
    CHECK_EQ(w.get(x, 62), f.ids.water);
    CHECK_EQ(w.get(x, 35), f.ids.oil);
  }
}

TEST(fire_ignites_oil_which_burns_away_into_smoke) {
  Fixture f;
  px::World w(2, 1, f.materials, 5);
  w.fill_rect(6, 40, 4, 24, f.ids.stone);     // basin walls
  w.fill_rect(110, 40, 4, 24, f.ids.stone);
  w.fill_rect(6, 60, 108, 4, f.ids.stone);
  w.fill_rect(10, 50, 100, 10, f.ids.oil);
  w.fill_rect(58, 46, 4, 4, f.ids.fire);      // a fire dropped onto the surface
  bool saw_smoke = false;
  for (int s = 0; s < 1500; ++s) {
    px::step(w);
    saw_smoke = saw_smoke || count_in(w, f.ids.smoke) > 0;
  }
  CHECK_EQ(count_in(w, f.ids.oil), 0);
  CHECK(saw_smoke);
}

TEST(burning_wood_eventually_goes_out_and_the_world_sleeps) {
  Fixture f;
  px::World w(2, 2, f.materials, 7);
  w.fill_rect(0, 120, 128, 8, f.ids.stone);
  w.fill_rect(40, 100, 30, 20, f.ids.wood);
  w.fill_rect(38, 116, 2, 4, f.ids.fire);
  int steps = 0;
  for (; steps < 20000 && (w.active_chunk_count() > 0 || steps < 10); ++steps) px::step(w);
  CHECK(steps < 20000);
  CHECK(count_in(w, f.ids.wood) < 30 * 20 / 4);  // most of the wood burned
  CHECK_EQ(count_in(w, f.ids.fire), 0);
  CHECK_EQ(count_in(w, f.ids.flame), 0);
  CHECK_EQ(count_in(w, f.ids.smoke), 0);
  // Everything has cooled back to near ambient. Temperatures are whole
  // degrees and a 1-degree gap does not flow, so buried layers settle up to a
  // degree per layer warm (here 8 layers of floor); air is exactly ambient.
  int worst = 0;
  for (int y = 0; y < w.height(); ++y)
    for (int x = 0; x < w.width(); ++x) {
      worst = std::max(worst, std::abs(w.temperature(x, y) - px::kAmbient));
      if (w.get(x, y) == px::kEmpty) CHECK_EQ(w.temperature(x, y), px::kAmbient);
    }
  CHECK(worst <= 10);
}

TEST(lava_meeting_water_makes_stone_and_steam) {
  Fixture f;
  px::World w(1, 1, f.materials, 9);
  w.fill_rect(0, 56, 64, 8, f.ids.water);
  w.fill_rect(28, 30, 8, 8, f.ids.lava);
  const int stone_before = count_in(w, f.ids.stone);
  bool saw_steam = false;
  for (int s = 0; s < 400; ++s) {
    px::step(w);
    saw_steam = saw_steam || count_in(w, f.ids.steam) > 0;
  }
  CHECK(count_in(w, f.ids.stone) > stone_before);
  CHECK(saw_steam);
}

TEST(heat_conducts_along_stone) {
  Fixture f;
  px::World w(1, 1, f.materials, 11);
  w.fill_rect(0, 30, 64, 6, f.ids.stone);
  w.fill_rect(0, 31, 2, 4, f.ids.lava);  // heat source sealed into the bar's left end
  run(w, 600);
  // Heat diffuses: hot near the source, warm ~12 cells away after 10 seconds,
  // still ambient further out, and monotonically falling in between.
  CHECK(w.temperature(4, 32) > 400);
  CHECK(w.temperature(12, 32) > px::kAmbient);
  CHECK_EQ(w.temperature(40, 32), px::kAmbient);
  for (int x = 3; x < 40; ++x) CHECK(w.temperature(x, 32) >= w.temperature(x + 1, 32));
}

// Steam cools into cloud, which gathers under the lid and rains where it is
// dense. A thin remnant too sparse to rain stays as cloud (humidity).
TEST(steam_in_a_closed_box_condenses_back_to_water) {
  Fixture f;
  px::World w(1, 1, f.materials, 13);
  stone_box(w, f.ids.stone, 10, 10, 50, 50);
  w.fill_rect(15, 15, 30, 10, f.ids.steam);
  const int steam_before = count_in(w, f.ids.steam);
  run(w, 6000);
  CHECK_EQ(count_in(w, f.ids.steam), 0);
  CHECK_EQ(count_in(w, f.ids.water) + count_in(w, f.ids.cloud), steam_before);  // none lost
  CHECK(count_in(w, f.ids.water) > steam_before * 2 / 3);                      // most of it rained
}

TEST(water_boils_over_a_heat_source) {
  Fixture f;
  px::World w(1, 1, f.materials, 15);
  w.fill_rect(10, 60, 44, 4, f.ids.lava);
  w.fill_rect(10, 58, 44, 2, f.ids.stone);  // stove plate keeps lava and water apart
  w.fill_rect(10, 50, 44, 8, f.ids.water);
  bool saw_steam = false;
  for (int s = 0; s < 1200 && !saw_steam; ++s) {
    px::step(w);
    saw_steam = count_in(w, f.ids.steam) > 0;
  }
  CHECK(saw_steam);
}

TEST(ice_melts_next_to_heat) {
  Fixture f;
  px::World w(1, 1, f.materials, 17);
  w.fill_rect(20, 40, 10, 10, f.ids.ice);
  w.fill_rect(30, 40, 3, 10, f.ids.stone);
  w.fill_rect(33, 40, 6, 10, f.ids.lava);
  run(w, 3000);
  CHECK(count_in(w, f.ids.ice) < 100 / 2);
  CHECK(count_in(w, f.ids.water) > 0);
}

TEST(chunk_sleeping_matches_updating_everything_with_heat_and_fire) {
  Fixture f;
  auto build = [&](px::World& w) {
    const int H = w.height();
    w.fill_rect(0, H - 8, w.width(), 8, f.ids.stone);
    w.fill_rect(10, H - 30, 60, 22, f.ids.oil);
    for (int k = 0; k < 4; ++k) w.fill_rect(14 + k * 14, H - 90, 4, 60, f.ids.wood);
    w.fill_rect(8, H - 34, 4, 4, f.ids.fire);
    w.fill_rect(120, H - 40, 70, 32, f.ids.water);
    w.fill_rect(140, 20, 20, 20, f.ids.lava);
    w.fill_rect(200, H - 50, 30, 42, f.ids.ice);
    w.fill_rect(90, 60, 30, 10, f.ids.steam);
  };
  px::World chunked(4, 3, f.materials, 0xF1E);
  px::World full(4, 3, f.materials, 0xF1E);
  build(chunked);
  build(full);
  int diverged_at = -1;
  for (int s = 0; s < 900 && diverged_at < 0; ++s) {
    px::step(chunked);
    full.wake_all();
    px::step(full);
    for (int y = 0; y < full.height() && diverged_at < 0; ++y)
      for (int x = 0; x < full.width(); ++x)
        if (chunked.get(x, y) != full.get(x, y) || chunked.temperature(x, y) != full.temperature(x, y)) {
          diverged_at = s + 1;
          break;
        }
  }
  CHECK_EQ(diverged_at, -1);
}

// The README's example: a new material and reaction added purely as data,
// with no engine changes.
TEST(new_material_from_data_only_acid_eats_stone) {
  Fixture f;
  px::MaterialDef acid;
  acid.name = "acid";
  acid.phase = px::Phase::Liquid;
  acid.density = 1100;
  acid.color = {120, 220, 60};
  acid.dispersion = 6;
  const px::MaterialId acid_id = f.materials.add(acid);
  f.materials.add_reaction("acid", "stone", "empty", "smoke", 0.05f);
  f.materials.finalize();

  px::World w(1, 1, f.materials, 21);
  w.fill_rect(0, 40, 64, 24, f.ids.stone);
  w.fill_rect(20, 30, 24, 10, acid_id);
  const int stone_before = count_in(w, f.ids.stone);
  run(w, 1500);
  CHECK(count_in(w, f.ids.stone) < stone_before - 100);  // it dissolved a hole
  CHECK(count_in(w, acid_id) < 240);                       // and was used up doing it
}

// ------------------------------------------------------------------ step 3:
// horizontal wrapping

TEST(wrapping_world_maps_every_x_onto_the_ring) {
  Fixture f;
  px::World w(2, 1, f.materials, 1, px::EdgeX::Wrap);
  CHECK(w.wraps_x());
  CHECK(w.in_bounds(-1, 10));
  CHECK(w.in_bounds(w.width() + 5, 10));
  CHECK(!w.in_bounds(5, -1));  // top and bottom are still walls
  CHECK(!w.in_bounds(5, w.height()));
  w.set(-1, 10, f.ids.stone);  // writes the last column
  CHECK_EQ(w.get(w.width() - 1, 10), f.ids.stone);
  w.set(w.width() * 3 + 2, 11, f.ids.sand);  // several laps around
  CHECK_EQ(w.get(2, 11), f.ids.sand);
  CHECK_EQ(w.get(2 - w.width() * 5, 11), f.ids.sand);
  CHECK_EQ(w.get(5, -1), px::kOutOfBounds);
  CHECK_EQ(w.wrap_x(-1), w.width() - 1);
  CHECK_EQ(w.wrap_x(w.width()), 0);
  // The same cell through a different lap gets the same colour shade.
  px::World a(2, 1, f.materials, 9, px::EdgeX::Wrap), b(2, 1, f.materials, 9, px::EdgeX::Wrap);
  a.set(3, 3, f.ids.sand);
  b.set(3 + b.width(), 3, f.ids.sand);
  CHECK_EQ(a.chunk_for(3, 3).shade(px::World::local_index(3, 3)), b.chunk_for(3, 3).shade(px::World::local_index(3, 3)));
}

TEST(walled_world_keeps_its_side_walls) {
  Fixture f;
  px::World w(2, 1, f.materials, 1);
  CHECK(!w.wraps_x());
  CHECK_EQ(w.get(-1, 10), px::kOutOfBounds);
  CHECK_EQ(w.get(w.width(), 10), px::kOutOfBounds);
}

TEST(sand_slides_across_the_seam) {
  Fixture f;
  px::World w(2, 1, f.materials, 4, px::EdgeX::Wrap);
  w.fill_rect(0, 63, w.width(), 1, f.ids.stone);
  w.fill_rect(0, 20, 1, 43, f.ids.sand);  // a column right at x = 0
  run_until_settled(w, 3000);
  int left = 0, right = 0;  // sand that ended up across the seam vs. on the near side
  for (int y = 0; y < 63; ++y) {
    for (int x = w.width() - 20; x < w.width(); ++x) left += w.get(x, y) == f.ids.sand ? 1 : 0;
    for (int x = 1; x < 20; ++x) right += w.get(x, y) == f.ids.sand ? 1 : 0;
  }
  CHECK(left > 5);  // the pile spread onto the far side of the seam
  CHECK(right > 5);
  CHECK_EQ(w.count(f.ids.sand), std::size_t{43});
}

TEST(water_levels_around_the_whole_ring) {
  Fixture f;
  px::World w(3, 1, f.materials, 9, px::EdgeX::Wrap);
  w.fill_rect(0, 63, w.width(), 1, f.ids.stone);
  w.fill_rect(w.width() - 20, 10, 40, 40, f.ids.water);  // straddles the seam
  run(w, 3000);
  int min_col = 64, max_col = 0;
  for (int x = 0; x < w.width(); ++x) {
    int col = 0;
    for (int y = 0; y < w.height(); ++y) col += w.get(x, y) == f.ids.water ? 1 : 0;
    min_col = std::min(min_col, col);
    max_col = std::max(max_col, col);
  }
  CHECK(max_col - min_col <= 1);  // no wall at the seam: one flat sheet all the way round
  CHECK_EQ(w.count(f.ids.water), std::size_t{40 * 40});
  CHECK(run_until_settled(w, 3000) < 3000);
}

TEST(heat_flows_across_the_seam) {
  Fixture f;
  // A hot spot on the seam spreads exactly like one in the middle of the world.
  auto spread = [&](int hot_x, int dx) {
    px::World w(2, 1, f.materials, 2, px::EdgeX::Wrap);
    w.fill_rect(0, 30, w.width(), 3, f.ids.stone);
    w.set_temperature(hot_x, 31, 1000);
    run(w, 8);
    return w.temperature(hot_x + dx, 31);
  };
  CHECK(spread(0, -1) > 100);  // the far side of the seam warmed up
  CHECK(spread(0, 1) > 100);
  // Not bit-identical: the neighbour across the seam is visited at the other
  // end of the row scan, and heat exchange depends slightly on update order.
  CHECK(std::abs(spread(0, -1) - spread(64, -1)) <= 5);
  CHECK(std::abs(spread(0, 2) - spread(64, 2)) <= 3);
}

TEST(chunk_sleeping_matches_updating_everything_on_a_wrapping_world) {
  Fixture f;
  px::World chunked(4, 2, f.materials, 0xBEEF, px::EdgeX::Wrap);
  px::World full(4, 2, f.materials, 0xBEEF, px::EdgeX::Wrap);
  for (px::World* w : {&chunked, &full}) {
    w->fill_rect(0, w->height() - 6, w->width(), 6, f.ids.stone);
    w->fill_rect(w->width() - 40, 20, 80, 60, f.ids.water);  // across the seam
    w->fill_rect(-10, 0, 20, 15, f.ids.sand);
    w->fill_rect(100, 90, 20, 10, f.ids.oil);
    w->fill_rect(100, 100, 4, 4, f.ids.fire);
    w->fill_rect(w->width() - 3, 110, 6, 6, f.ids.lava);  // hot spot on the seam
  }
  bool identical = true;
  for (int s = 0; s < 400 && identical; ++s) {
    px::step(chunked);
    full.wake_all();
    px::step(full);
    for (int y = 0; y < full.height() && identical; ++y)
      for (int x = 0; x < full.width(); ++x)
        if (chunked.get(x, y) != full.get(x, y) || chunked.temperature(x, y) != full.temperature(x, y)) {
          identical = false;
          break;
        }
  }
  CHECK(identical);
}

// ------------------------------------------------------------------ region-limited stepping

TEST(chunks_outside_the_region_pause_and_resume) {
  Fixture f;
  px::World w(4, 2, f.materials, 31);
  w.set(10, 0, f.ids.sand);   // chunk (0, 0): inside the region
  w.set(200, 0, f.ids.sand);  // chunk (3, 0): outside
  const px::ChunkRegion near{0, 0, 1, 1};
  int paused = 0;
  for (int s = 0; s < 300; ++s) paused = px::step(w, near).paused_chunks;
  CHECK_EQ(w.get(10, w.height() - 1), f.ids.sand);  // fell all the way
  CHECK_EQ(w.get(200, 0), f.ids.sand);               // frozen in place, not lost
  CHECK(paused >= 1);
  for (int s = 0; s < 300; ++s) px::step(w);  // camera comes back: it resumes
  CHECK_EQ(w.get(200, w.height() - 1), f.ids.sand);
}

TEST(material_crossing_into_a_paused_chunk_waits_there) {
  Fixture f;
  px::World w(1, 3, f.materials, 33);
  w.set(20, 60, f.ids.sand);  // falls from chunk row 0 into paused row 1
  const px::ChunkRegion top{0, 0, 0, 0};
  for (int s = 0; s < 200; ++s) px::step(w, top);
  int found_y = -1;
  for (int y = 0; y < w.height(); ++y)
    if (w.get(20, y) == f.ids.sand) found_y = y;
  CHECK(found_y >= 64 && found_y < 128);  // entered the paused chunk and stopped
  CHECK_EQ(w.count(f.ids.sand), std::size_t{1});
  for (int s = 0; s < 300; ++s) px::step(w);
  CHECK_EQ(w.get(20, w.height() - 1), f.ids.sand);
}

TEST(region_wraps_across_the_seam) {
  Fixture f;
  px::World w(4, 1, f.materials, 35, px::EdgeX::Wrap);
  w.set(250, 0, f.ids.sand);  // last chunk column
  w.set(130, 0, f.ids.sand);  // chunk column 2, outside
  const px::ChunkRegion seam{-1, 0, 0, 0};  // columns 3 and 0
  for (int s = 0; s < 200; ++s) px::step(w, seam);
  CHECK_EQ(w.get(250, 63), f.ids.sand);
  CHECK_EQ(w.get(130, 0), f.ids.sand);
}

TEST(a_region_covering_everything_matches_plain_step) {
  Fixture f;
  auto build = [&](px::World& w) {
    w.fill_rect(0, 100, 256, 28, f.ids.stone);
    w.fill_rect(20, 10, 60, 40, f.ids.water);
    w.fill_rect(120, 10, 30, 30, f.ids.sand);
    w.fill_rect(180, 80, 10, 10, f.ids.lava);
    w.fill_rect(200, 70, 20, 30, f.ids.wood);
  };
  px::World a(4, 2, f.materials, 37), b(4, 2, f.materials, 37);
  build(a);
  build(b);
  const px::ChunkRegion all{0, 0, 3, 1};
  bool same = true;
  for (int s = 0; s < 400 && same; ++s) {
    px::step(a);
    px::step(b, all);
    for (int y = 0; y < a.height() && same; ++y)
      for (int x = 0; x < a.width(); ++x)
        if (a.get(x, y) != b.get(x, y) || a.temperature(x, y) != b.temperature(x, y)) {
          same = false;
          break;
        }
  }
  CHECK(same);
}

// ------------------------------------------------------------------ packed cells

TEST(packed_cell_fields_do_not_disturb_each_other) {
  px::Chunk c;
  const int i = px::Chunk::index(5, 7);
  CHECK_EQ(c.temperature(i), px::kAmbient);  // new chunks start at ambient
  c.set_shade(i, 13);
  c.set_temperature(i, -273);
  CHECK_EQ(c.temperature(i), -273);
  CHECK_EQ(c.shade(i), 13);
  c.set_temperature(i, px::Chunk::kMaxTemp);
  CHECK_EQ(c.temperature(i), px::Chunk::kMaxTemp);
  CHECK_EQ(c.shade(i), 13);
  c.set_temperature(i, 100000);  // clamped, not wrapped
  CHECK_EQ(c.temperature(i), px::Chunk::kMaxTemp);
  c.set_temperature(i, -5000);
  CHECK_EQ(c.temperature(i), px::Chunk::kMinTemp);
  CHECK(px::Chunk::kMaxTemp >= 3000);  // well above lava (1200)

  for (int v : {-120, -1, 0, 1, 8}) {
    for (int d : {-1, 0, 1}) {
      c.set_fall_speed(i, v);
      c.set_flow(i, d);
      CHECK_EQ(c.fall_speed(i), v);
      CHECK_EQ(c.flow(i), d);
      c.set_fall_speed(i, v);  // writing one half keeps the other
      CHECK_EQ(c.flow(i), d);
    }
  }
  c.set_life(i, 900);
  CHECK_EQ(c.life(i), 900);
}

TEST(update_bits_are_per_step) {
  px::Chunk c;
  const int i = 100;
  CHECK(!c.was_updated(i, 1));
  c.mark_updated(i, 1);
  CHECK(c.was_updated(i, 1));
  CHECK(!c.was_updated(i + 1, 1));
  CHECK(!c.was_updated(i, 2));  // a new step: everything is fresh again
  c.mark_updated(i + 1, 2);
  CHECK(!c.was_updated(i, 2));  // the old step's bit was cleared
  c.clear_updated(i + 1);
  CHECK(!c.was_updated(i + 1, 2));
  CHECK(!c.was_updated(i, 1 + 255));  // no wraparound, unlike a clock byte
}

TEST(registry_rejects_expiring_powders_and_liquids) {
  // Powders and liquids keep their motion where other materials keep a lifetime.
  px::MaterialRegistry reg;
  px::MaterialDef d;
  d.name = "fizz";
  d.phase = px::Phase::Liquid;
  d.density = 900;
  d.lifetime_min = d.lifetime_max = 60;
  reg.add(d);
  bool threw = false;
  try {
    reg.finalize();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  px::MaterialRegistry reg2;
  px::MaterialDef goo;
  goo.name = "goo";
  goo.phase = px::Phase::Liquid;
  reg2.add(goo);
  px::MaterialDef husk;
  husk.name = "husk";
  husk.phase = px::Phase::Solid;
  husk.high_temp = 100;
  husk.high_to = "goo";
  husk.burn_time = 50;  // a lifetime handed to a liquid
  reg2.add(husk);
  threw = false;
  try {
    reg2.finalize();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}
