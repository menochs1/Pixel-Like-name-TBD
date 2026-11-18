#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/material.hpp"
#include "core/world.hpp"

namespace px {

/// A rectangle of world cells to draw, top-left at (x, y). On a wrapping world
/// x may be anything (the view wraps around the seam); elsewhere, parts outside
/// the world are drawn as background. A zero width or height means the whole world.
struct Viewport {
  int x = 0, y = 0;
  int width = 0, height = 0;
};

/// Converts world cells to 32-bit ARGB pixels (0xAARRGGBB), one pixel per cell.
/// Knows nothing about windows or GPUs: the app uploads the buffer to a texture,
/// tools can write it to an image file.
class PixelRenderer {
 public:
  explicit PixelRenderer(const MaterialRegistry& materials);

  /// `pixels` must hold at least pitch * view height entries.
  /// Flickering materials (fire, lava) are animated from the world tick here;
  /// flicker is a visual effect, not simulation state.
  void render(const World& world, std::uint32_t* pixels, int pitch, Viewport view = {}) const;

  /// Heat map: blue below 0 °C, dark at ambient, through red and yellow to white
  /// at 1200 °C+. Materials are faintly outlined so the scene stays readable.
  void render_temperature(const World& world, std::uint32_t* pixels, int pitch, Viewport view = {}) const;

  /// Outlines every chunk's pending dirty rect (debug view of the active-chunk system).
  void draw_dirty_rects(const World& world, std::uint32_t* pixels, int pitch, Viewport view = {}) const;

 private:
  static constexpr int kShades = 16;
  std::vector<std::array<std::uint32_t, kShades>> palette_;
  std::vector<std::uint8_t> flicker_;  // per material
};

}  // namespace px
