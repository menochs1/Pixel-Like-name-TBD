#include "render/pixel_renderer.hpp"

#include <algorithm>

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

void PixelRenderer::render(const World& world, std::uint32_t* pixels, int pitch) const {
  for (int cy = 0; cy < world.height_chunks(); ++cy) {
    for (int cx = 0; cx < world.width_chunks(); ++cx) {
      const Chunk& c = world.chunk(cx, cy);
      for (int ly = 0; ly < kChunkSize; ++ly) {
        std::uint32_t* row = pixels + static_cast<std::ptrdiff_t>((cy << kChunkShift) + ly) * pitch + (cx << kChunkShift);
        for (int lx = 0; lx < kChunkSize; ++lx) {
          const int i = Chunk::index(lx, ly);
          row[lx] = palette_[c.material[i]][c.shade[i] & (kShades - 1)];
        }
      }
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
