// Sandbox tools: heat/cool brushes, the pixel inspector, and the text it is
// drawn with.

#include <algorithm>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "render/text.hpp"
#include "sandbox/tools.hpp"
#include "test_framework.hpp"

namespace {

struct ToolFixture {
  px::MaterialRegistry materials;
  px::BaseMaterials ids;
  ToolFixture() : ids(px::register_base_materials(materials)) {}
};

bool contains(const std::vector<std::string>& lines, const std::string& text) {
  return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}

}  // namespace

TEST(heat_brush_is_strongest_at_the_centre_and_stays_inside_its_radius) {
  ToolFixture f;
  px::World w(1, 1, f.materials, 1);
  w.fill_rect(0, 0, 64, 64, f.ids.stone);
  px::apply_thermal_brush(w, 32, 32, 6, px::kHeatBrush);
  CHECK(w.temperature(32, 32) > w.temperature(36, 32));
  CHECK(w.temperature(36, 32) > px::kAmbient);
  CHECK_EQ(w.temperature(39, 32), px::kAmbient);  // outside the radius
  CHECK(w.active_chunk_count() > 0);              // heated cells are woken
  for (int i = 0; i < 500; ++i) px::apply_thermal_brush(w, 32, 32, 6, px::kHeatBrush);
  CHECK(w.temperature(32, 32) <= px::kHeatBrush.target);  // approaches, never overshoots
  CHECK(w.temperature(32, 32) > 2900);
}

TEST(cool_brush_freezes_water) {
  ToolFixture f;
  px::World w(1, 1, f.materials, 2);
  w.fill_rect(0, 40, 64, 24, f.ids.water);
  for (int i = 0; i < 120; ++i) {
    px::apply_thermal_brush(w, 32, 50, 6, px::kCoolBrush);
    px::step(w);
  }
  CHECK(w.count(f.ids.ice) > 20);
}

TEST(heat_brush_melts_rock_and_cool_brush_hardens_it_again) {
  ToolFixture f;
  px::World w(2, 2, f.materials, 3);
  w.fill_rect(0, 0, 128, 128, f.ids.stone);
  for (int i = 0; i < 60; ++i) {
    px::apply_thermal_brush(w, 64, 64, 8, px::kHeatBrush);
    px::step(w);
  }
  const int lava = w.count(f.ids.lava) > 0 ? static_cast<int>(w.count(f.ids.lava)) : 0;
  CHECK(lava > 20);  // rock passed 1500 C and melted
  for (int i = 0; i < 120; ++i) {
    px::apply_thermal_brush(w, 64, 64, 12, px::kCoolBrush);
    px::step(w);
  }
  CHECK(static_cast<int>(w.count(f.ids.lava)) < lava / 4);  // most of it hardened back into rock
}

TEST(cool_brush_hardens_lava_and_puts_out_fire) {
  ToolFixture f;
  px::World w(1, 1, f.materials, 4);
  w.fill_rect(0, 56, 64, 8, f.ids.stone);
  w.fill_rect(10, 50, 20, 6, f.ids.lava);
  w.fill_rect(40, 45, 10, 10, f.ids.fire);
  const int lava_before = static_cast<int>(w.count(f.ids.lava));
  for (int i = 0; i < 20; ++i) {
    px::apply_thermal_brush(w, 20, 52, 12, px::kCoolBrush);
    px::apply_thermal_brush(w, 45, 50, 12, px::kCoolBrush);
    px::step(w);
  }
  CHECK(static_cast<int>(w.count(f.ids.lava)) < lava_before / 2);
  CHECK(w.count(f.ids.fire) < 10);  // out long before its natural lifetime
}

// Heat sources are only released when something cools them hard: ordinary
// conduction never pulls lava below its hardening point.
TEST(lava_left_alone_stays_lava) {
  ToolFixture f;
  px::World w(1, 1, f.materials, 5);
  w.fill_rect(0, 56, 64, 8, f.ids.stone);
  w.fill_rect(10, 50, 20, 6, f.ids.lava);
  w.set(50, 55, f.ids.lava);  // a lone drop on cold rock
  const auto lava = w.count(f.ids.lava);
  for (int i = 0; i < 600; ++i) px::step(w);
  CHECK_EQ(w.count(f.ids.lava), lava);
}

TEST(thermal_brush_wraps_across_the_seam) {
  ToolFixture f;
  px::World w(2, 1, f.materials, 6, px::EdgeX::Wrap);
  w.fill_rect(0, 0, w.width(), 64, f.ids.stone);
  px::apply_thermal_brush(w, 0, 30, 5, px::kHeatBrush);
  CHECK(w.temperature(w.width() - 2, 30) > px::kAmbient);
  CHECK(w.temperature(2, 30) > px::kAmbient);
  CHECK_EQ(w.temperature(w.width() - 2, 30), w.temperature(2, 30));  // symmetric
}

TEST(inspector_reports_what_is_under_the_cursor) {
  ToolFixture f;
  px::World w(1, 1, f.materials, 7, px::EdgeX::Wrap);
  w.set(10, 10, f.ids.stone);
  w.set_temperature(10, 10, 345);
  w.damage(10, 10, 50);
  px::CellInfo stone = px::inspect(w, 10 + w.width(), 10);  // wraps
  CHECK(stone.in_world);
  CHECK_EQ(stone.x, 10);
  CHECK_EQ(stone.material, f.ids.stone);
  CHECK(!stone.derived);
  CHECK_EQ(stone.temperature, 345);
  CHECK(stone.health_percent < 80);
  CHECK(stone.health_percent > 70);
  CHECK(stone.has_high);
  CHECK_EQ(stone.transition_to, std::string("lava"));
  CHECK(stone.active);  // just changed: its chunk will simulate
  const std::vector<std::string> lines = px::describe(w, stone);
  CHECK(contains(lines, "stone"));
  CHECK(contains(lines, "temp 345 C"));
  CHECK(contains(lines, "above 1500 C: lava"));
  CHECK(contains(lines, "of 200"));

  CHECK(!px::inspect(w, 5, -1).in_world);
  CHECK(contains(px::describe(w, px::inspect(w, 5, -1)), "outside"));
  CHECK(contains(px::describe(w, px::inspect(w, 30, 30)), "empty"));
}

TEST(inspector_shows_derived_materials_and_their_origin) {
  ToolFixture f;
  px::World w(1, 1, f.materials, 8);
  w.fill_rect(0, 56, 64, 8, f.ids.stone);
  w.fill_rect(0, 50, 64, 6, f.ids.sand);
  w.fill_rect(0, 44, 64, 6, f.ids.oil);
  for (int i = 0; i < 400; ++i) px::step(w);
  int tx = -1, ty = -1;
  for (int y = 0; y < 64 && tx < 0; ++y)
    for (int x = 0; x < 64; ++x)
      if (f.materials.props(w.get(x, y)).base == f.ids.tar && w.get(x, y) != f.ids.tar) {
        tx = x;
        ty = y;
        break;
      }
  CHECK(tx >= 0);
  const px::CellInfo tar = px::inspect(w, tx, ty);
  CHECK(tar.derived);
  CHECK_EQ(tar.base, f.ids.tar);
  CHECK(contains(px::describe(w, tar), "derived tar"));
}

TEST(every_character_the_inspector_prints_has_a_glyph) {
  ToolFixture f;
  px::World w(1, 1, f.materials, 9);
  const px::MaterialId all[] = {f.ids.stone, f.ids.sand,  f.ids.water, f.ids.oil,  f.ids.wood,  f.ids.fire,
                                f.ids.flame, f.ids.smoke, f.ids.steam, f.ids.lava, f.ids.ice,   f.ids.dirt,
                                f.ids.grass, f.ids.coal,  f.ids.leaves, f.ids.basalt, f.ids.tar, px::kEmpty};
  std::string missing;
  for (const px::MaterialId m : all) {
    w.set(20, 20, m);
    w.set_temperature(20, 20, -40);
    for (const std::string& line : px::describe(w, px::inspect(w, 20, 20)))
      for (const char c : line)
        if (!px::has_glyph(c)) missing += c;
  }
  CHECK_EQ(missing, std::string());
}

TEST(text_draws_glyphs_and_clips_to_the_buffer) {
  CHECK_EQ(px::text_width(""), 0);
  CHECK_EQ(px::text_width("A"), 3);
  CHECK_EQ(px::text_width("AB"), 7);
  CHECK_EQ(px::text_width("AB", 2), 14);
  CHECK(px::has_glyph('a'));  // lower case draws as capitals
  CHECK(!px::has_glyph('~'));

  std::vector<std::uint32_t> buf(8 * 8, 0);
  px::draw_text(buf.data(), 8, 8, 8, 1, 1, "A", 0xFFFFFFFFU);
  // A = .#. / #.# / ### / #.# / #.#
  const char* expect[5] = {".#.", "#.#", "###", "#.#", "#.#"};
  bool ok = true;
  for (int r = 0; r < 5; ++r)
    for (int k = 0; k < 3; ++k) ok = ok && ((buf[static_cast<std::size_t>((1 + r) * 8 + 1 + k)] != 0) == (expect[r][k] == '#'));
  CHECK(ok);
  CHECK_EQ(buf[0], 0U);  // nothing outside the glyph

  // Drawing partly or wholly off the buffer must not write outside it.
  std::vector<std::uint32_t> guarded(10 * 10 + 20, 0xABCDEF01U);
  std::uint32_t* inner = guarded.data() + 10;
  for (int i = 0; i < 100; ++i) inner[i] = 0;
  px::draw_text(inner, 10, 10, 10, -2, -3, "WWWW", 1U, 2);
  px::draw_text(inner, 10, 10, 10, 8, 8, "HELLO", 1U, 3);
  px::draw_text_panel(inner, 10, 10, 10, -5, 6, {"PANEL", "TEXT"});
  bool guard_ok = true;
  for (int i = 0; i < 10; ++i) guard_ok = guard_ok && guarded[static_cast<std::size_t>(i)] == 0xABCDEF01U && guarded[static_cast<std::size_t>(110 + i)] == 0xABCDEF01U;
  CHECK(guard_ok);
}
