// Weather: steam rising into a cloud layer, rain from dense cloud, and wind.

#include <algorithm>
#include <cmath>

#include "core/material.hpp"
#include "core/rng.hpp"
#include "core/simulator.hpp"
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

/// Mean x of the cells holding `m`.
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

}  // namespace

TEST(steam_rises_into_the_cloud_layer_and_stays_there) {
  WeatherFixture f;
  px::World w(2, 3, f.materials, 1);
  px::Weather sky;
  sky.clouds = true;
  sky.cloud_top = 40;
  w.set_weather(sky);
  w.fill_rect(0, 184, w.width(), 8, f.ids.stone);
  w.fill_rect(40, 150, 40, 10, f.ids.steam);
  const int vapour = count(w, f.ids.steam);
  run(w, 2000);
  CHECK_EQ(count(w, f.ids.steam), 0);  // all cooled into cloud...
  int highest = w.height(), lowest = 0;
  for (int y = 0; y < w.height(); ++y)
    for (int x = 0; x < w.width(); ++x)
      if (w.get(x, y) == f.ids.cloud) {
        highest = std::min(highest, y);
        lowest = std::max(lowest, y);
      }
  CHECK(highest >= 40);  // ...which floats at the layer, not above it
  CHECK(lowest < 70);    // ...and does not hang about lower down
  CHECK_EQ(count(w, f.ids.cloud) + count(w, f.ids.water), vapour);
}

TEST(dense_cloud_rains_and_water_is_conserved) {
  WeatherFixture f;
  px::World w(2, 2, f.materials, 2);
  px::Weather sky;
  sky.clouds = true;
  sky.cloud_top = 20;
  w.set_weather(sky);
  w.fill_rect(0, 120, w.width(), 8, f.ids.stone);
  w.fill_rect(30, 20, 60, 10, f.ids.cloud);  // a thick cloud
  const int cloud = count(w, f.ids.cloud);
  run(w, 3000);
  int on_ground = 0;
  for (int x = 0; x < w.width(); ++x) on_ground += w.get(x, 119) == f.ids.water ? 1 : 0;
  CHECK(on_ground > 20);  // it rained
  CHECK_EQ(count(w, f.ids.cloud) + count(w, f.ids.water), cloud);
}

// Thin cloud only drizzles: a drop now and then, nothing like a thick cloud.
TEST(thin_cloud_only_drizzles) {
  WeatherFixture f;
  px::World w(2, 2, f.materials, 3);
  px::Weather sky;
  sky.clouds = true;
  sky.cloud_top = 20;
  w.set_weather(sky);
  w.fill_rect(0, 120, w.width(), 8, f.ids.stone);
  for (int x = 10; x < 110; x += 3) w.set(x, 20, f.ids.cloud);  // 34 wisps
  run(w, 600);
  CHECK(count(w, f.ids.water) < 10);  // ~4 expected; a thick cloud of that size rains most of itself
  CHECK(count(w, f.ids.water) + count(w, f.ids.cloud) == 34);
}

TEST(wind_blows_smoke_and_cloud_downwind) {
  WeatherFixture f;
  for (const float bias : {0.3f, -0.3f}) {
    px::World w(4, 2, f.materials, 4);
    px::Weather sky;
    sky.wind_bias = bias;  // steady wind, no wandering
    w.set_weather(sky);
    w.fill_rect(120, 100, 16, 16, f.ids.smoke);
    w.fill_rect(120, 20, 16, 4, f.ids.cloud);
    const double smoke0 = mean_x(w, f.ids.smoke), cloud0 = mean_x(w, f.ids.cloud);
    run(w, 120);
    const double smoke_moved = mean_x(w, f.ids.smoke) - smoke0, cloud_moved = mean_x(w, f.ids.cloud) - cloud0;
    CHECK(bias > 0 ? smoke_moved > 15 : smoke_moved < -15);
    CHECK(bias > 0 ? cloud_moved > 20 : cloud_moved < -20);
  }
}

// A cloud in the wind travels as one body: on the steps the wind moves it,
// every cell moves, so it does not smear out.
TEST(clouds_travel_as_one) {
  WeatherFixture f;
  px::World w(6, 1, f.materials, 5, px::EdgeX::Wrap);
  px::Weather sky;
  sky.clouds = true;
  sky.cloud_top = 10;
  sky.wind_bias = 0.25f;
  w.set_weather(sky);
  w.fill_rect(40, 10, 30, 2, f.ids.cloud);  // two rows: too thin to rain
  run(w, 400);
  int left = w.width(), right = 0, n = 0;
  for (int x = 0; x < w.width(); ++x)
    for (int y = 0; y < w.height(); ++y)
      if (w.get(x, y) == f.ids.cloud) {
        left = std::min(left, x);
        right = std::max(right, x);
        ++n;
      }
  CHECK_EQ(n + count(w, f.ids.water), 60);  // a few cells drizzle out on the way
  CHECK(n > 45);
  CHECK(left > 80);              // it travelled (0.25 cells/step * 400 = ~100 cells)
  CHECK(right - left + 1 < 45);  // and is still about 30 wide
}

TEST(rain_drifts_with_the_wind) {
  WeatherFixture f;
  px::World w(4, 2, f.materials, 6);
  px::Weather sky;
  sky.wind_bias = 0.3f;
  w.set_weather(sky);
  w.fill_rect(0, 120, w.width(), 8, f.ids.stone);
  for (int i = 0; i < 20; ++i) w.set(100 + i, 5 + (i % 3) * 3, f.ids.water);  // a sprinkle of drops
  run(w, 40);  // just as they land (afterwards the puddle spreads both ways)
  CHECK(mean_x(w, f.ids.water) > 115);  // ~10 cells downwind of where they fell from (109.5)
}

TEST(wind_is_calm_underground_and_strongest_up_high) {
  WeatherFixture f;
  px::World w(2, 4, f.materials, 7);
  px::Weather sky;
  sky.wind_bias = 0.3f;
  sky.ground_y = 200;
  w.set_weather(sky);
  CHECK_EQ(w.wind_at(210), 0.0f);  // below ground level
  CHECK(std::fabs(w.wind_at(60) - 0.3f) < 1e-6f);  // well above it
  CHECK(w.wind_at(170) > 0.0f);
  CHECK(w.wind_at(170) < w.wind_at(60));  // building up near the ground
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
  CHECK(same);              // the same seed, the same weather
  CHECK(differs);           // another seed, other weather
  CHECK(hi - lo > 0.2f);    // it changes over time
  CHECK(hi <= 0.4f + 1e-6f);
  CHECK(lo >= -0.4f - 1e-6f);
}

// Chunked simulation stays identical to updating every cell with clouds,
// rain and a wandering wind.
TEST(weather_keeps_chunked_simulation_identical) {
  WeatherFixture f;
  for (int seed = 1; seed <= 3; ++seed) {
    const px::EdgeX edge = seed % 2 ? px::EdgeX::Wrap : px::EdgeX::Wall;
    px::World a(4, 3, f.materials, static_cast<std::uint64_t>(seed), edge);
    px::World b(4, 3, f.materials, static_cast<std::uint64_t>(seed), edge);
    px::Weather sky;
    sky.clouds = true;
    sky.cloud_top = 30;
    sky.max_wind = 0.4f;
    sky.wind_period = 300;
    sky.ground_y = 150;
    for (px::World* w : {&a, &b}) {
      w->set_weather(sky);
      w->fill_rect(0, 160, w->width(), 32, f.ids.stone);
      w->fill_rect(20, 120, 60, 40, f.ids.water);
      w->fill_rect(150, 30, 60, 12, f.ids.cloud);
      w->fill_rect(100, 140, 10, 10, f.ids.lava);  // boils the water: steam, cloud, rain
    }
    bool same = true;
    for (int s = 0; s < 600 && same; ++s) {
      if (s % 50 == 0) {
        a.fill_circle(30 + s % 150, 100, 4, f.ids.steam, 0.7f);
        b.fill_circle(30 + s % 150, 100, 4, f.ids.steam, 0.7f);
      }
      px::step(a);
      b.wake_all();
      px::step(b);
      for (int y = 0; y < a.height() && same; ++y)
        for (int x = 0; x < a.width() && same; ++x)
          same = a.get(x, y) == b.get(x, y) && a.temperature(x, y) == b.temperature(x, y);
    }
    CHECK(same);
  }
}
