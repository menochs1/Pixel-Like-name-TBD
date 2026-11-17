// Stress test for the simulation core.
//
//   pixel_stress [--quick]
//
// 1. Worst-case load scenarios (whole world in motion, a 10-million-cell world),
//    timed per step against the 16.7 ms frame budget.
// 2. Randomised editing fuzz: brush strokes of every size (including far out of
//    bounds), clears and scene rebuilds interleaved with steps, checking after
//    every step that material is conserved and per-cell data stays valid.
// 3. A long run past the 8-bit update-clock wraparound.
// Exits non-zero if any invariant breaks. Build with sanitizers for memory checks.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/rng.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "world/demo_scene.hpp"

namespace {

int g_failures = 0;

void fail(const std::string& msg) {
  std::printf("  FAIL: %s\n", msg.c_str());
  ++g_failures;
}

struct Counts {
  std::vector<std::size_t> per_material;
  bool operator==(const Counts&) const = default;
};

Counts count_all(const px::World& w) {
  Counts c;
  c.per_material.assign(w.materials().size(), 0);
  for (int cy = 0; cy < w.height_chunks(); ++cy)
    for (int cx = 0; cx < w.width_chunks(); ++cx)
      for (px::MaterialId m : w.chunk(cx, cy).material) {
        if (m >= c.per_material.size()) {
          fail("invalid material id " + std::to_string(m));
          return c;
        }
        ++c.per_material[m];
      }
  return c;
}

bool cells_valid(const px::World& w) {
  for (int cy = 0; cy < w.height_chunks(); ++cy)
    for (int cx = 0; cx < w.width_chunks(); ++cx) {
      const px::Chunk& c = w.chunk(cx, cy);
      for (int i = 0; i < px::kChunkArea; ++i) {
        if (c.velocity_y[i] < 0 || c.velocity_y[i] > 8) return false;
        if (c.velocity_x[i] < -1 || c.velocity_x[i] > 1) return false;
        if (c.shade[i] > 15) return false;
      }
    }
  return true;
}

struct Timing {
  double mean = 0, p95 = 0, max = 0;
};

Timing time_steps(px::World& w, int steps) {
  std::vector<double> t;
  t.reserve(static_cast<std::size_t>(steps));
  for (int s = 0; s < steps; ++s) {
    const auto t0 = std::chrono::steady_clock::now();
    px::step(w);
    t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  Timing r;
  for (double v : t) r.mean += v;
  r.mean /= steps;
  std::sort(t.begin(), t.end());
  r.p95 = t[(t.size() - 1) * 95 / 100];
  r.max = t.back();
  return r;
}

void load_scenario(const char* name, int wc, int hc, int steps, const px::MaterialRegistry& reg,
                   const std::function<void(px::World&)>& build) {
  px::World w(wc, hc, reg, 0xC0FFEE);
  build(w);
  const Counts before = count_all(w);
  const Timing t = time_steps(w, steps);
  const bool conserved = count_all(w) == before;
  const bool fits = t.p95 < 16.7;
  std::printf("  %-34s %5dx%-5d mean %7.2f ms  p95 %7.2f  max %7.2f  %s%s\n", name, w.width(), w.height(), t.mean,
              t.p95, t.max, fits ? "within 60 fps" : "OVER 60 fps budget", conserved ? "" : "  MASS NOT CONSERVED");
  if (!conserved) fail(std::string(name) + ": mass not conserved");
}

}  // namespace

int main(int argc, char** argv) {
  const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
  px::MaterialRegistry reg;
  const px::BaseMaterials ids = px::register_base_materials(reg);

  // ---------------------------------------------------------------- load
  std::printf("Load scenarios (%s):\n", quick ? "quick" : "full");
  const int steps = quick ? 60 : 300;
  load_scenario("app world, all sand falling", 8, 5, steps, reg, [&](px::World& w) {
    w.fill_rect(0, 0, w.width(), w.height() - 4, ids.sand);
    w.fill_rect(0, w.height() - 4, w.width(), 4, px::kEmpty);
    // Hollow out the bottom so everything above must fall.
    w.fill_rect(0, w.height() - 120, w.width(), 120, px::kEmpty);
  });
  load_scenario("app world, all water sloshing", 8, 5, steps, reg, [&](px::World& w) {
    w.fill_rect(0, 0, w.width() / 2, w.height(), ids.water);
  });
  load_scenario("app world, sand/water checkerboard", 8, 5, steps, reg, [&](px::World& w) {
    for (int y = 0; y < w.height() - 60; ++y)
      for (int x = 0; x < w.width(); ++x) w.set(x, y, ((x / 4 + y / 4) & 1) ? ids.sand : ids.water);
  });
  load_scenario("2048x1152 rain (benchmark scene)", 32, 18, steps, reg,
                [&](px::World& w) { px::build_rain_scene(w, ids); });
  if (!quick) {
    load_scenario("10M-cell world, 1/3 in motion", 64, 38, 120, reg, [&](px::World& w) {
      w.fill_rect(0, w.height() - 16, w.width(), 16, ids.stone);
      for (int y = 0; y < w.height() / 3; ++y)
        for (int x = 0; x < w.width(); ++x) {
          const std::uint32_t r = px::cell_hash(1, x, y, 0) & 0xFF;
          if (r < 100) w.set(x, y, ids.sand);
          else if (r < 170) w.set(x, y, ids.water);
        }
    });
  }

  // ---------------------------------------------------------------- fuzz
  const int ops = quick ? 3000 : 20000;
  std::printf("\nEditing fuzz: %d random operations on the app-sized world\n", ops);
  {
    px::World w(8, 5, reg, 0xC0FFEE);
    px::build_demo_scene(w, ids);
    const px::MaterialId palette[] = {px::kEmpty, ids.stone, ids.sand, ids.water};
    std::uint32_t seed = 12345;
    auto rnd = [&](int n) {
      seed = px::hash32(seed + 0x9E3779B9U);
      return static_cast<int>(seed % static_cast<std::uint32_t>(n));
    };
    int steps_run = 0;
    bool ok = true;
    for (int op = 0; op < ops && ok; ++op) {
      const int kind = rnd(100);
      if (kind < 1) {
        w.clear();
      } else if (kind < 2) {
        px::build_demo_scene(w, ids);
      } else if (kind < 70) {
        // Brush stroke, deliberately reaching well outside the world.
        const int x = rnd(w.width() + 400) - 200;
        const int y = rnd(w.height() + 400) - 200;
        const int r = rnd(10) == 0 ? rnd(200) : rnd(40);
        w.fill_circle(x, y, r, palette[rnd(4)], rnd(2) ? 1.0f : 0.55f);
      } else {
        const Counts before = count_all(w);
        const int n = 1 + rnd(8);
        for (int s = 0; s < n; ++s) px::step(w);
        steps_run += n;
        if (!(count_all(w) == before)) {
          fail("mass changed during simulation at op " + std::to_string(op));
          ok = false;
        }
        if (!cells_valid(w)) {
          fail("invalid per-cell data at op " + std::to_string(op));
          ok = false;
        }
      }
    }
    std::printf("  %d operations, %d simulation steps: %s\n", ops, steps_run, ok ? "all invariants held" : "BROKEN");
  }

  // ---------------------------------------------------------------- long run
  const int long_steps = quick ? 2000 : 30000;
  std::printf("\nLong run: %d steps (update clock wraps every 255) with periodic pours\n", long_steps);
  {
    px::World w(8, 5, reg, 7);
    px::build_demo_scene(w, ids);
    for (int s = 0; s < long_steps; ++s) {
      if (s % 500 == 0) w.fill_circle(100 + (s / 500) * 37 % 300, 20, 10, (s / 500) % 2 ? ids.sand : ids.water, 0.6f);
      px::step(w);
    }
    const Counts c = count_all(w);
    // Grains must come to rest: nothing floating with empty space below it.
    int floating = 0;
    for (int y = 0; y < w.height() - 1; ++y)
      for (int x = 0; x < w.width(); ++x)
        if (w.get(x, y) == ids.sand && w.get(x, y + 1) == px::kEmpty) ++floating;
    std::printf("  tick %u, sand %zu, water %zu, stuck-in-air sand %d, valid data %s\n", w.tick(),
                c.per_material[ids.sand], c.per_material[ids.water], floating, cells_valid(w) ? "yes" : "NO");
    if (floating > 0) fail("sand left floating after long run");
    if (!cells_valid(w)) fail("invalid per-cell data after long run");
  }

  std::printf("\n%s\n", g_failures ? "STRESS TEST FAILED" : "STRESS TEST PASSED");
  return g_failures ? 1 : 0;
}
