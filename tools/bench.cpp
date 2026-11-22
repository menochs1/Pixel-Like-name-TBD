// Headless benchmark and snapshot tool.
//
//   pixel_bench [--scene rain|demo|gen] [--seed N] [--scale S] [--steps N] [--ppm out.ppm]
//
// Runs the simulation without a window, reports per-step timing and activity,
// and can write the final frame as a binary PPM image. `gen` generates the
// app's 4096x1536 wrapping world from --seed and also reports how long that took.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "render/pixel_renderer.hpp"
#include "world/demo_scene.hpp"
#include "world/world_gen.hpp"

namespace {

bool write_ppm(const std::string& path, const std::vector<std::uint32_t>& argb, int w, int h) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  std::vector<unsigned char> row(static_cast<std::size_t>(w) * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::uint32_t p = argb[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
      row[static_cast<std::size_t>(x) * 3 + 0] = static_cast<unsigned char>(p >> 16);
      row[static_cast<std::size_t>(x) * 3 + 1] = static_cast<unsigned char>(p >> 8);
      row[static_cast<std::size_t>(x) * 3 + 2] = static_cast<unsigned char>(p);
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  return std::fclose(f) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scene = "rain";
  std::string ppm;
  int steps = 600;
  std::uint64_t seed = 0xC0FFEEULL;
  float scale = 1.0f;  // gen: a world this many times larger, with terrain features to match
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) scene = argv[++i];
    else if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--ppm") == 0 && i + 1 < argc) ppm = argv[++i];
    else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
    else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) scale = static_cast<float>(std::atof(argv[++i]));
    else {
      std::fprintf(stderr, "usage: %s [--scene rain|demo|gen] [--seed N] [--scale S] [--steps N] [--ppm out.ppm]\n", argv[0]);
      return 2;
    }
  }

  px::MaterialRegistry materials;
  const px::BaseMaterials ids = px::register_base_materials(materials);
  const bool rain = scene == "rain";
  const bool gen = scene == "gen";
  const int gs = std::max(1, static_cast<int>(scale + 0.5f));
  px::World world(gen ? 64 * gs : (rain ? 32 : 8), gen ? 24 * gs : (rain ? 18 : 5), materials, seed,
                  gen ? px::EdgeX::Wrap : px::EdgeX::Wall);
  if (gen) {
    const auto t0 = std::chrono::steady_clock::now();
    px::WorldGenParams params;
    params.scale = scale;
    px::generate_world(world, ids, params);
    std::printf("generated in %.0f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  } else if (rain) {
    px::build_rain_scene(world, ids);
  } else {
    px::build_demo_scene(world, ids);
  }

  const std::size_t sand0 = world.count(ids.sand);
  const std::size_t water0 = world.count(ids.water);
  std::printf("scene=%s world=%dx%d (%d chunks, %.1f MiB cells) sand=%zu water=%zu\n", scene.c_str(), world.width(),
              world.height(), world.width_chunks() * world.height_chunks(),
              static_cast<double>(world.width_chunks() * world.height_chunks()) * sizeof(px::Chunk) / (1024.0 * 1024.0),
              sand0, water0);

  std::vector<double> times;
  times.reserve(static_cast<std::size_t>(steps));
  std::int64_t moved = 0, scanned = 0, peak_moved = 0;
  long active = 0;
  for (int s = 0; s < steps; ++s) {
    const auto t0 = std::chrono::steady_clock::now();
    const px::StepStats st = px::step(world);
    times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    moved += st.cells_moved;
    scanned += st.cells_scanned;
    active += st.active_chunks;
    peak_moved = std::max(peak_moved, st.cells_moved);
  }

  std::vector<double> sorted = times;
  std::sort(sorted.begin(), sorted.end());
  double sum = 0;
  for (double t : times) sum += t;
  const auto pct = [&](double p) { return sorted[static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1))]; };
  std::printf("steps=%d  mean=%.3f ms  p50=%.3f  p95=%.3f  max=%.3f\n", steps, sum / steps, pct(0.5), pct(0.95),
              sorted.back());
  std::printf("avg per step: %.0f cells scanned, %.0f cells moved (peak %lld), %.1f active chunks\n",
              static_cast<double>(scanned) / steps, static_cast<double>(moved) / steps,
              static_cast<long long>(peak_moved), static_cast<double>(active) / steps);
  // Generated worlds have lava meeting water, which legitimately converts mass.
  const bool conserved = gen || (world.count(ids.sand) == sand0 && world.count(ids.water) == water0);
  if (gen) std::printf("final active chunks: %d of %d\n", world.active_chunk_count(), world.width_chunks() * world.height_chunks());
  else std::printf("mass conserved: %s\n", conserved ? "yes" : "NO");

  if (!ppm.empty()) {
    px::PixelRenderer renderer(materials);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(world.width()) * static_cast<std::size_t>(world.height()));
    renderer.render(world, pixels.data(), world.width());
    if (!write_ppm(ppm, pixels, world.width(), world.height())) {
      std::fprintf(stderr, "failed to write %s\n", ppm.c_str());
      return 1;
    }
    std::printf("wrote %s\n", ppm.c_str());
  }
  return conserved ? 0 : 1;
}
