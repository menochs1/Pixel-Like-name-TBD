// Climate and weather: colder air higher up, steam feeding regional rain,
// rain and snow falling in from above, wind, and the crowding rule.

#include <algorithm>
#include <cmath>

#include "core/material.hpp"
#include "core/rng.hpp"
#include "core/simulator.hpp"
#include "core/weather.hpp"
#include "core/world.hpp"
#include "test_framework.hpp"

namespace {

struct WeatherFixture {
  px::MaterialRegistry materials;
  px::BaseMaterials ids;
  WeatherFixture() : ids(px::register_base_materials(materials)) {}
};

void run(px::World& w, int steps) {
  for (int i = 0; i < steps; ++i) px::step(w);
}

int count(const px::World& w, px::MaterialId m) { return static_cast<int>(w.count(m)); }

double mean_x(const px::World& w, px::MaterialId m) {
  double sum = 0;
  int n = 0;
  for (int y = 0; y < w.height(); ++y)
    for (int x = 0; x < w.width(); ++x)
      if (w.get(x, y) == m) {
        sum += x;
        ++n;
      }
  return n ? sum / n : -1;
}

/// Ground level at row 200, colder above, rain areas of 128 columns.
px::Weather test_weather() {
  px::Weather sky;
  sky.ground_y = 200;
  sky.lapse_rate = 0.1f;  // steep, so tests stay small: freezing 200 rows up
  sky.vapour_y = 40;
  sky.region_width = 128;
  sky.rain_start = 400.0f;
  sky.rain_stop = 50.0f;
  sky.rain_per_step = 3.0f;
  return sky;
}

px::ChunkRegion everything(const px::World& w) { return {0, 0, w.width_chunks() - 1, w.height_chunks() - 1}; }

}  // namespace

TEST(it_gets_colder_higher_up) {
  WeatherFixture f;
  px::World w(2, 4, f.materials, 1);
  w.set_weather(test_weather());
  CHECK_EQ(w.ambient_at(250), px::kAmbient);  // underground
  CHECK_EQ(w.ambient_at(200), px::kAmbient);  // ground level
  CHECK_EQ(w.ambient_at(100), px::kAmbient - 10);
  CHECK_EQ(w.ambient_at(0), px::kAmbient - 20);
  w.set(10, 50, f.ids.stone);  // placed things start at the air's temperature there
  CHECK_EQ(w.temperature(10, 50), w.ambient_at(50));
  w.set(10, 52, f.ids.lava);   // unless they have their own
  CHECK_EQ(w.temperature(10, 52), 1200);
}

// Air left at 20 C up high cools to the cold of its altitude and then sleeps:
// a sky at rest costs nothing.
TEST(the_sky_settles_into_its_cold_and_sleeps) {
  WeatherFixture f;
  px::World w(2, 4, f.materials, 2);
  w.set_weather(test_weather());
  w.fill_rect(0, 200, w.width(), 56, f.ids.stone);
  w.wake_all();  // made before the weather: every cell of air is at 20 C, off its altitude's temperature
  int steps = 0;
  while (steps < 3000 && (w.active_chunk_count() > 0 || steps < 5)) {
    px::step(w);
    ++steps;
  }
  CHECK(steps < 3000);
  bool settled = true;
  for (int y = 0; y < 200; y += 7) settled = settled && std::abs(w.temperature(60, y) - w.ambient_at(y)) <= 1;
  CHECK(settled);
}

TEST(water_freezes_up_high_but_not_down_low) {
  WeatherFixture f;
  px::World w(2, 4, f.materials, 3);
  px::Weather sky = test_weather();
  sky.lapse_rate = 0.15f;  // the high ledge is at about -7 C
  w.set_weather(sky);
  w.settle_temperatures();
  for (const int top : {12, 182}) {  // a basin high up and one low down
    const int x0 = top == 12 ? 8 : 68;
    w.fill_rect(x0, top + 8, 24, 2, f.ids.stone);
    w.fill_rect(x0, top, 2, 8, f.ids.stone);
    w.fill_rect(x0 + 22, top, 2, 8, f.ids.stone);
    w.fill_rect(x0 + 2, top + 2, 20, 6, f.ids.water);
  }
  run(w, 1500);
  int ice_high = 0, water_low = 0;
  for (int y = 0; y < 30; ++y)
    for (int x = 0; x < 40; ++x) ice_high += w.get(x, y) == f.ids.ice ? 1 : 0;
  for (int y = 170; y < 192; ++y)
    for (int x = 60; x < 100; ++x) water_low += w.get(x, y) == f.ids.water ? 1 : 0;  // 20 C down there
  CHECK(ice_high > 60);
  CHECK_EQ(water_low, 120);
}

// Steam that rises out of sight is not lost: it becomes moisture in the
// weather over where it rose.
TEST(steam_rising_out_of_sight_becomes_moisture) {
  WeatherFixture f;
  px::World w(4, 4, f.materials, 4);
  px::Weather sky = test_weather();
  sky.lapse_rate = 0.0f;  // keep it simple: no extra cooling
  w.set_weather(sky);
  for (int a = 0; a < w.weather_areas(); ++a) w.moisture(a) = 0.0f;
  w.fill_rect(140, 150, 30, 10, f.ids.steam);  // over area 1 (columns 128..255)
  const int steam = count(w, f.ids.steam);
  run(w, 1500);
  float moisture = 0;
  for (int a = 0; a < w.weather_areas(); ++a) moisture += w.moisture(a);
  CHECK(moisture > 20.0f);                    // some made it up there
  CHECK(w.moisture(1) >= moisture * 0.8f);    // mostly over where it rose (steam drifts a little)
  CHECK_EQ(count(w, f.ids.steam) + count(w, f.ids.water) + static_cast<int>(moisture), steam);  // none lost
  bool none_above = true;
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < w.width(); ++x) none_above = none_above && w.get(x, y) != f.ids.steam;
  CHECK(none_above);
}

TEST(a_wet_area_rains_from_above_until_it_has_dried_out) {
  WeatherFixture f;
  px::World w(8, 4, f.materials, 5);  // 512 wide: four weather areas
  w.set_weather(test_weather());
  w.fill_rect(0, 200, w.width(), 56, f.ids.stone);
  for (int a = 0; a < w.weather_areas(); ++a) w.moisture(a) = 0.0f;
  w.moisture(2) = 600.0f;  // area 2: columns 256..383
  // Drops appear along the top, only over area 2.
  px::update_weather(w, everything(w));
  int in_area = 0, elsewhere = 0;
  for (int x = 0; x < w.width(); ++x)
    if (w.get(x, 0) == f.ids.water) (x >= 256 && x < 384 ? in_area : elsewhere) += 1;
  CHECK(in_area > 0);
  CHECK_EQ(elsewhere, 0);
  int drops = in_area, max_raining = 0;
  for (int s = 0; s < 600; ++s) {
    const px::WeatherStats st = px::update_weather(w, everything(w));
    drops += st.drops;
    max_raining = std::max(max_raining, st.raining_areas);
    px::step(w);
  }
  CHECK_EQ(max_raining, 1);
  CHECK(drops > 100);
  CHECK(!w.raining(2));                  // it rained itself out...
  CHECK(w.moisture(2) <= 50.0f);         // ...down to rain_stop
  CHECK_EQ(count(w, f.ids.water), drops);  // and every drop that fell is on the ground
}

TEST(humidity_builds_up_until_it_rains) {
  WeatherFixture f;
  px::World w(2, 4, f.materials, 6);
  px::Weather sky = test_weather();
  sky.humidity_rate = 2.0f;
  w.set_weather(sky);
  for (int a = 0; a < w.weather_areas(); ++a) w.moisture(a) = 0.0f;
  int first_rain = -1;
  for (int s = 0; s < 400 && first_rain < 0; ++s)
    if (px::update_weather(w, everything(w)).raining_areas > 0) first_rain = s;
  CHECK(first_rain >= 195);  // 400 / 2 per step
  CHECK(first_rain <= 205);
}

TEST(snow_falls_where_the_air_is_freezing) {
  WeatherFixture f;
  px::World w(2, 4, f.materials, 7);
  px::Weather sky = test_weather();
  sky.lapse_rate = 0.15f;  // the top row is at -10 C
  w.set_weather(sky);
  w.settle_temperatures();
  w.fill_rect(0, 200, w.width(), 56, f.ids.stone);
  w.moisture(0) = 600.0f;
  for (int s = 0; s < 20; ++s) {
    px::update_weather(w, everything(w));
    px::step(w);
  }
  CHECK(count(w, f.ids.snow) > 10);    // snow up in the cold
  CHECK_EQ(count(w, f.ids.water), 0);  // (lower down, where it is above 0 C, it melts into rain)
}

TEST(no_rain_falls_into_caves) {
  WeatherFixture f;
  px::World w(2, 6, f.materials, 8);
  w.set_weather(test_weather());
  w.moisture(0) = 600.0f;
  const px::ChunkRegion underground{0, 4, 1, 5};  // its top row (256) is below ground level (200)
  int drops = 0;
  for (int s = 0; s < 100; ++s) drops += px::update_weather(w, underground).drops;
  CHECK_EQ(drops, 0);
  CHECK(w.moisture(0) < 600.0f);  // it still rained up there, unseen
}

TEST(wet_weather_drifts_with_the_wind) {
  WeatherFixture f;
  px::World w(8, 4, f.materials, 9, px::EdgeX::Wrap);  // four weather areas
  px::Weather sky = test_weather();
  sky.rain_start = 1e9f;  // never rain: watch the moisture move
  sky.wind_bias = 0.3f;
  w.set_weather(sky);
  for (int a = 0; a < w.weather_areas(); ++a) w.moisture(a) = 0.0f;
  w.moisture(0) = 1000.0f;
  for (int s = 0; s < 300; ++s) px::update_weather(w, everything(w));
  CHECK(w.moisture(1) > 100.0f);         // carried downwind
  CHECK(w.moisture(3) < w.moisture(1));  // not upwind
  float total = 0;
  for (int a = 0; a < w.weather_areas(); ++a) total += w.moisture(a);
  CHECK(std::fabs(total - 1000.0f) < 1.0f);  // moved, not made or lost
}

TEST(wind_blows_smoke_downwind) {
  WeatherFixture f;
  for (const float bias : {0.3f, -0.3f}) {
    px::World w(4, 2, f.materials, 4);
    px::Weather sky;
    sky.wind_bias = bias;
    w.set_weather(sky);
    w.fill_rect(120, 100, 16, 16, f.ids.smoke);
    const double smoke0 = mean_x(w, f.ids.smoke);
    run(w, 120);
    const double moved = mean_x(w, f.ids.smoke) - smoke0;
    CHECK(bias > 0 ? moved > 15 : moved < -15);
  }
}

TEST(rain_and_snow_drift_with_the_wind) {
  WeatherFixture f;
  for (const px::MaterialId m : {f.ids.water, f.ids.snow}) {
    px::World w(4, 2, f.materials, 6);
    px::Weather sky;
    sky.wind_bias = 0.3f;
    w.set_weather(sky);
    w.fill_rect(0, 120, w.width(), 8, f.ids.stone);
    for (int i = 0; i < 20; ++i) w.set(100 + i, 5 + (i % 3) * 3, m);
    run(w, 40);  // just as they land
    CHECK(mean_x(w, m) > 115);  // ~10 cells downwind of where they fell from (109.5)
  }
}

TEST(wind_is_calm_underground_and_strongest_up_high) {
  WeatherFixture f;
  px::World w(2, 4, f.materials, 7);
  px::Weather sky;
  sky.wind_bias = 0.3f;
  sky.ground_y = 200;
  w.set_weather(sky);
  CHECK_EQ(w.wind_at(210), 0.0f);
  CHECK(std::fabs(w.wind_at(60) - 0.3f) < 1e-6f);
  CHECK(w.wind_at(170) > 0.0f);
  CHECK(w.wind_at(170) < w.wind_at(60));
}

TEST(wind_wanders_but_is_deterministic) {
  WeatherFixture f;
  px::World a(1, 1, f.materials, 8), b(1, 1, f.materials, 8), c(1, 1, f.materials, 9);
  px::Weather sky;
  sky.max_wind = 0.4f;
  sky.wind_period = 600;
  for (px::World* w : {&a, &b, &c}) w->set_weather(sky);
  float lo = 1, hi = -1;
  bool same = true, differs = false;
  for (int t = 0; t < 6000; ++t) {
    px::step(a);
    px::step(b);
    px::step(c);
    lo = std::min(lo, a.wind_at(0));
    hi = std::max(hi, a.wind_at(0));
    same = same && a.wind_at(0) == b.wind_at(0);
    differs = differs || a.wind_at(0) != c.wind_at(0);
  }
  CHECK(same);
  CHECK(differs);
  CHECK(hi - lo > 0.2f);
  CHECK(hi <= 0.4f + 1e-6f);
  CHECK(lo >= -0.4f - 1e-6f);
}

TEST(crowding_rule_turns_dense_clusters_into_something_else) {
  px::MaterialRegistry reg;
  px::MaterialDef mist;
  mist.name = "mist";
  mist.phase = px::Phase::Solid;  // stays put, so only the rule acts
  mist.crowd_count = 6;
  mist.crowd_to = "dew";
  mist.crowd_chance = 1.0f;
  reg.add(mist);
  px::MaterialDef dew;
  dew.name = "dew";
  dew.phase = px::Phase::Solid;
  reg.add(dew);
  reg.finalize();
  px::World w(1, 1, reg, 1);
  w.fill_rect(10, 10, 5, 5, reg.require("mist"));  // a dense block
  for (int x = 30; x < 50; x += 2) w.set(x, 30, reg.require("mist"));  // sparse dots
  for (int i = 0; i < 3; ++i) px::step(w);
  CHECK(w.count(reg.require("dew")) >= 3);       // the crowded middle condensed
  CHECK_EQ(w.get(10, 10), reg.require("mist"));  // a corner has only 3 neighbours
  CHECK_EQ(w.get(30, 30), reg.require("mist"));  // and the sparse dots none
}

// Chunked simulation stays identical to updating every cell with cold air,
// vapour leaving, rain and snow falling and a wandering wind.
TEST(weather_keeps_chunked_simulation_identical) {
  WeatherFixture f;
  for (int seed = 1; seed <= 3; ++seed) {
    const px::EdgeX edge = seed % 2 ? px::EdgeX::Wrap : px::EdgeX::Wall;
    px::World a(4, 4, f.materials, static_cast<std::uint64_t>(seed), edge);
    px::World b(4, 4, f.materials, static_cast<std::uint64_t>(seed), edge);
    px::Weather sky = test_weather();
    sky.max_wind = 0.4f;
    sky.wind_period = 300;
    sky.humidity_rate = 1.0f;
    for (px::World* w : {&a, &b}) {
      w->set_weather(sky);
      w->settle_temperatures();  // the sky starts at rest, as in a generated world
      w->fill_rect(0, 200, w->width(), 56, f.ids.stone);
      w->fill_rect(20, 170, 60, 30, f.ids.water);
      w->fill_rect(100, 190, 10, 10, f.ids.lava);  // boils the water: steam rises, joins the weather
      w->moisture(1) = 600.0f;                     // and it rains on area 1
    }
    bool same = true;
    for (int s = 0; s < 500 && same; ++s) {
      px::update_weather(a, everything(a));
      px::update_weather(b, everything(b));
      px::step(a);
      b.wake_all();
      px::step(b);
      for (int y = 0; y < a.height() && same; ++y)
        for (int x = 0; x < a.width() && same; ++x)
          same = a.get(x, y) == b.get(x, y) && a.temperature(x, y) == b.temperature(x, y);
      for (int k = 0; k < a.weather_areas() && same; ++k) same = a.moisture(k) == b.moisture(k);
    }
    CHECK(same);
  }
}
