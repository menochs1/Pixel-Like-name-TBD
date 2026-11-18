#include "render/pixel_renderer.hpp"

#include <algorithm>
#include <iterator>

#include "core/rng.hpp"

namespace px {
namespace {

std::uint32_t argb(int r, int g, int b) {
  auto clamp = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
  return 0xFF000000U | (clamp(r) << 16) | (clamp(g) << 8) | clamp(b);
}

}  // namespace

namespace {
std::uint32_t heat_color(int t);
}  // namespace

PixelRenderer::PixelRenderer(const MaterialRegistry& materials) {
  heat_lut_.resize(static_cast<std::size_t>(kHeatLutMax - kHeatLutMin + 1));
  for (int t = kHeatLutMin; t <= kHeatLutMax; ++t) heat_lut_[static_cast<std::size_t>(t - kHeatLutMin)] = heat_color(t);
  palette_.resize(materials.size());
  flicker_.resize(materials.size());
  for (std::size_t id = 0; id < materials.size(); ++id) {
    const MaterialDef& def = materials.def(static_cast<MaterialId>(id));
    flicker_[id] = def.flicker ? 1 : 0;
    for (int s = 0; s < kShades; ++s) {
      // Map shade 0..15 to -variation..+variation brightness.
      const int offset = (s * 2 - (kShades - 1)) * def.color_variation / (kShades - 1);
      palette_[id][static_cast<std::size_t>(s)] = argb(def.color.r + offset, def.color.g + offset, def.color.b + offset);
    }
  }
}

namespace {

/// Blends `c` toward an incandescent orange-white as temperature rises above
/// ~450 °C, so hot stone, sand or metal visibly glow.
std::uint32_t glow(std::uint32_t c, int t) {
  if (t <= 450) return c;
  const int f = std::min(220, (t - 450) * 220 / 900);  // 0..220 of 256
  const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  const int gr = 255, gg = 96 + std::min(140, (t - 450) / 6), gb = 24;
  return argb(r + (gr - r) * f / 256, g + (gg - g) * f / 256, b + (gb - b) * f / 256);
}

std::uint32_t heat_color(int t) {
  struct Stop {
    int t, r, g, b;
  };
  static constexpr Stop kStops[] = {{-40, 80, 160, 255}, {0, 30, 60, 140}, {20, 12, 12, 18},
                                    {100, 90, 20, 110}, {300, 200, 30, 30}, {700, 255, 150, 20},
                                    {1200, 255, 255, 220}};
  if (t <= kStops[0].t) return argb(kStops[0].r, kStops[0].g, kStops[0].b);
  for (std::size_t k = 1; k < std::size(kStops); ++k) {
    const Stop& a = kStops[k - 1];
    const Stop& b = kStops[k];
    if (t <= b.t) {
      const int f = (t - a.t) * 256 / (b.t - a.t);
      return argb(a.r + (b.r - a.r) * f / 256, a.g + (b.g - a.g) * f / 256, a.b + (b.b - a.b) * f / 256);
    }
  }
  return argb(255, 255, 220);
}

}  // namespace

namespace {

constexpr std::uint32_t kOutside = 0xFF050508U;  // beyond the edges of a walled world

Viewport resolve(const World& world, Viewport v) {
  if (v.width <= 0 || v.height <= 0) return {0, 0, world.width(), world.height()};
  return v;
}

/// Calls fn(screen_x, world_x, n) for each run of up to n consecutive cells of
/// view row that lie in one chunk; world_x is in [0, width). Cells outside a
/// walled world are skipped (the caller pre-fills them).
template <typename Fn>
void for_each_run(const World& world, const Viewport& v, Fn&& fn) {
  int sx = 0;
  while (sx < v.width) {
    int wx = v.x + sx;
    if (world.wraps_x()) {
      wx = world.wrap_x(wx);
    } else if (wx < 0 || wx >= world.width()) {
      ++sx;
      continue;
    }
    const int n = std::min(v.width - sx, kChunkSize - (wx & kChunkMask));
    const int limit = world.wraps_x() ? n : std::min(n, world.width() - wx);
    fn(sx, wx, limit);
    sx += limit;
  }
}

}  // namespace

void PixelRenderer::render(const World& world, std::uint32_t* pixels, int pitch, Viewport view) const {
  const Viewport v = resolve(world, view);
  const std::uint32_t tick = world.tick();
  for (int sy = 0; sy < v.height; ++sy) {
    std::uint32_t* row = pixels + static_cast<std::ptrdiff_t>(sy) * pitch;
    const int wy = v.y + sy;
    if (wy < 0 || wy >= world.height()) {
      std::fill(row, row + v.width, kOutside);
      continue;
    }
    if (!world.wraps_x()) std::fill(row, row + v.width, kOutside);
    for_each_run(world, v, [&](int sx, int wx, int n) {
      const Chunk& c = world.chunk_for(wx, wy);
      const int base = World::local_index(wx, wy);
      for (int k = 0; k < n; ++k) {
        const int i = base + k;
        const MaterialId m = c.material[i];
        std::uint32_t shade = c.shade[i];
        if (flicker_[m]) {
          shade = hash32(static_cast<std::uint32_t>(wx + k) * 0x9E3779B1U ^ static_cast<std::uint32_t>(wy) * 0x85EBCA77U ^
                         tick * 0xC2B2AE3DU);
        }
        const std::uint32_t col = palette_[m][shade & (kShades - 1)];
        row[sx + k] = m == kEmpty ? col : glow(col, c.temperature[i]);
      }
    });
  }
}

void PixelRenderer::render_temperature(const World& world, std::uint32_t* pixels, int pitch, Viewport view) const {
  const Viewport v = resolve(world, view);
  for (int sy = 0; sy < v.height; ++sy) {
    std::uint32_t* row = pixels + static_cast<std::ptrdiff_t>(sy) * pitch;
    const int wy = v.y + sy;
    if (wy < 0 || wy >= world.height()) {
      std::fill(row, row + v.width, kOutside);
      continue;
    }
    if (!world.wraps_x()) std::fill(row, row + v.width, kOutside);
    for_each_run(world, v, [&](int sx, int wx, int n) {
      const Chunk& c = world.chunk_for(wx, wy);
      const int base = World::local_index(wx, wy);
      for (int k = 0; k < n; ++k) {
        const int t = std::clamp<int>(c.temperature[base + k], kHeatLutMin, kHeatLutMax);
        std::uint32_t col = heat_lut_[static_cast<std::size_t>(t - kHeatLutMin)];
        if (c.material[base + k] != kEmpty) col |= 0x00202020U;  // lift solids/fluids a little
        row[sx + k] = col;
      }
    });
  }
}

void PixelRenderer::draw_dirty_rects(const World& world, std::uint32_t* pixels, int pitch, Viewport view) const {
  const Viewport v = resolve(world, view);
  const std::uint32_t color = 0xFFFF4060U;
  auto put = [&](int x, int y) {
    int sx = x - v.x;
    if (world.wraps_x()) {
      sx %= world.width();
      if (sx < 0) sx += world.width();
    }
    const int sy = y - v.y;
    if (sx < 0 || sy < 0 || sx >= v.width || sy >= v.height) return;
    pixels[static_cast<std::ptrdiff_t>(sy) * pitch + sx] = color;
  };
  for (int cy = 0; cy < world.height_chunks(); ++cy) {
    for (int cx = 0; cx < world.width_chunks(); ++cx) {
      const DirtyRect& r = world.chunk(cx, cy).next_dirty;
      if (r.empty()) continue;
      const int x0 = (cx << kChunkShift) + r.min_x;
      const int x1 = (cx << kChunkShift) + r.max_x;
      const int y0 = (cy << kChunkShift) + r.min_y;
      const int y1 = (cy << kChunkShift) + r.max_y;
      for (int x = x0; x <= x1; ++x) {
        put(x, y0);
        put(x, y1);
      }
      for (int y = y0; y <= y1; ++y) {
        put(x0, y);
        put(x1, y);
      }
    }
  }
}

}  // namespace px
