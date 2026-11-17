#include "render/pixel_renderer.hpp"

#include <algorithm>
#include <iterator>

namespace px {
namespace {

std::uint32_t argb(int r, int g, int b) {
  auto clamp = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
  return 0xFF000000U | (clamp(r) << 16) | (clamp(g) << 8) | clamp(b);
}

}  // namespace

PixelRenderer::PixelRenderer(const MaterialRegistry& materials) {
  palette_.resize(materials.size());
  for (std::size_t id = 0; id < materials.size(); ++id) {
    const MaterialDef& def = materials.def(static_cast<MaterialId>(id));
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

void PixelRenderer::render(const World& world, std::uint32_t* pixels, int pitch) const {
  for (int cy = 0; cy < world.height_chunks(); ++cy) {
    for (int cx = 0; cx < world.width_chunks(); ++cx) {
      const Chunk& c = world.chunk(cx, cy);
      for (int ly = 0; ly < kChunkSize; ++ly) {
        std::uint32_t* row = pixels + static_cast<std::ptrdiff_t>((cy << kChunkShift) + ly) * pitch + (cx << kChunkShift);
        for (int lx = 0; lx < kChunkSize; ++lx) {
          const int i = Chunk::index(lx, ly);
          const MaterialId m = c.material[i];
          const std::uint32_t base = palette_[m][c.shade[i] & (kShades - 1)];
          row[lx] = m == kEmpty ? base : glow(base, c.temperature[i]);
        }
      }
    }
  }
}

void PixelRenderer::render_temperature(const World& world, std::uint32_t* pixels, int pitch) const {
  for (int y = 0; y < world.height(); ++y) {
    for (int x = 0; x < world.width(); ++x) {
      const Chunk& c = world.chunk_for(x, y);
      const int i = World::local_index(x, y);
      std::uint32_t col = heat_color(c.temperature[i]);
      if (c.material[i] != kEmpty) col |= 0x00202020U;  // lift solids/fluids a little
      pixels[static_cast<std::ptrdiff_t>(y) * pitch + x] = col;
    }
  }
}

void PixelRenderer::draw_dirty_rects(const World& world, std::uint32_t* pixels, int pitch) const {
  const std::uint32_t color = 0xFFFF4060U;
  auto put = [&](int x, int y) { pixels[static_cast<std::ptrdiff_t>(y) * pitch + x] = color; };
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
