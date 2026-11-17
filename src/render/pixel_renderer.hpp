#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/material.hpp"
#include "core/world.hpp"

namespace px {

/// Converts world cells to 32-bit ARGB pixels (0xAARRGGBB), one pixel per cell.
/// Knows nothing about windows or GPUs: the app uploads the buffer to a texture,
/// tools can write it to an image file.
class PixelRenderer {
 public:
  explicit PixelRenderer(const MaterialRegistry& materials);

  /// `pixels` must hold at least pitch * world.height() entries.
  void render(const World& world, std::uint32_t* pixels, int pitch) const;

  /// Heat map: blue below 0 °C, dark at ambient, through red and yellow to white
  /// at 1200 °C+. Materials are faintly outlined so the scene stays readable.
  void render_temperature(const World& world, std::uint32_t* pixels, int pitch) const;

  /// Outlines every chunk's pending dirty rect (debug view of the active-chunk system).
  void draw_dirty_rects(const World& world, std::uint32_t* pixels, int pitch) const;

 private:
  static constexpr int kShades = 16;
  std::vector<std::array<std::uint32_t, kShades>> palette_;
};

}  // namespace px
