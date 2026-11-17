#include "world/demo_scene.hpp"

#include <algorithm>
#include <cmath>

#include "core/rng.hpp"

namespace px {
namespace {

/// 1D value noise in [0, 1], smoothly interpolated between integer lattice points.
float value_noise(std::uint64_t seed, float x) {
  const int x0 = static_cast<int>(std::floor(x));
  const float t = x - static_cast<float>(x0);
  const float s = t * t * (3.0f - 2.0f * t);
  const float a = static_cast<float>(cell_hash(seed, x0, 0, 0) & 0xFFFF) / 65535.0f;
  const float b = static_cast<float>(cell_hash(seed, x0 + 1, 0, 0) & 0xFFFF) / 65535.0f;
  return a + (b - a) * s;
}

}  // namespace

void build_demo_scene(World& world, const BaseMaterials& ids) {
  world.clear();
  const int w = world.width();
  const int h = world.height();
  const std::uint64_t seed = world.seed();

  // Rolling ground: two octaves of noise.
  for (int x = 0; x < w; ++x) {
    const float n = 0.65f * value_noise(seed, static_cast<float>(x) / 90.0f) +
                    0.35f * value_noise(seed ^ 0x51, static_cast<float>(x) / 23.0f);
    const int ground = h - 28 - static_cast<int>(n * 42.0f);
    for (int y = ground; y < h; ++y) world.set(x, y, ids.stone);
  }

  // A basin on the left third, filled with water.
  const int bx0 = w / 10;
  const int bx1 = w / 3;
  const int floor_y = h - 14;
  for (int x = bx0; x <= bx1; ++x) {
    for (int y = h - 90; y < floor_y; ++y) world.set(x, y, kEmpty);
  }
  world.fill_rect(bx0 - 4, h - 100, 4, floor_y - (h - 100), ids.stone);
  world.fill_rect(bx1 + 1, h - 100, 4, floor_y - (h - 100), ids.stone);
  world.fill_rect(bx0, h - 60, bx1 - bx0 + 1, floor_y - (h - 60), ids.water);

  // A ledge on the right holding a sand dune that spills over the edge.
  const int lx0 = w * 3 / 5;
  const int lx1 = w - 40;
  const int ledge_y = h / 3;
  world.fill_rect(lx0, ledge_y, lx1 - lx0, 6, ids.stone);
  for (int x = lx0 + 10; x < lx1 - 4; ++x) {
    const float t = static_cast<float>(x - lx0) / static_cast<float>(lx1 - lx0);
    const int dune = static_cast<int>(std::sin(t * 3.14159f) * 34.0f) + 4;
    world.fill_rect(x, ledge_y - dune, 1, dune, ids.sand);
  }

  // Floating platforms in the middle.
  for (int i = 0; i < 3; ++i) {
    const int px = w * (2 + i) / 8 + 20;
    const int py = h / 4 + i * 22;
    world.fill_rect(px, py, 36, 4, ids.stone);
  }
}

void build_rain_scene(World& world, const BaseMaterials& ids) {
  world.clear();
  const int w = world.width();
  const int h = world.height();
  world.fill_rect(0, h - 16, w, 16, ids.stone);
  for (int x = 64; x < w - 64; x += 160) world.fill_rect(x, h - 220, 80, 6, ids.stone);

  const std::uint64_t seed = world.seed();
  for (int y = 40; y < 140; ++y) {
    for (int x = 16; x < w - 16; ++x) {
      const std::uint32_t r = cell_hash(seed, x, y, 7) & 0xFF;
      if (r < 90) world.set(x, y, ids.sand);
      else if (r < 150) world.set(x, y, ids.water);
    }
  }
}

}  // namespace px
