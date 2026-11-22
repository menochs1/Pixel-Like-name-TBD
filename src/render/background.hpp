#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace px {

/// A painted backdrop behind the world, drawn wherever the world is empty:
/// a sky gradient and three silhouette layers (far mountains, hills, a near
/// wooded ridge) that move slower than the world the further away they are,
/// and a rock wall behind caves underground. Purely visual: it knows nothing
/// of the simulation.
class ParallaxBackground {
 public:
  /// `ground_y`: the world's mean ground level, where the layers' horizon
  /// sits when the camera looks at it. `surface`: the ground height of a world
  /// column (e.g. WorldGenerator::surface_y); below it, empty cells are caves
  /// and show the rock wall. Without it, everything below ground_y is.
  ParallaxBackground(std::uint64_t seed, int ground_y, std::function<int(int)> surface = {});

  /// Fills a view `w` x `h` whose top-left is world cell (cam_x, cam_y).
  void render(std::uint32_t* pixels, int pitch, int w, int h, double cam_x, double cam_y) const;

 private:
  struct Layer {
    float parallax;  // how fast it moves relative to the world (1 = with it)
    std::uint32_t colour;
    std::vector<std::int16_t> height;  // silhouette height above its horizon, per column, repeating
  };
  int surface_at(int x) const;

  int ground_y_;
  std::function<int(int)> surface_;
  mutable std::vector<int> surface_cache_;  // the generator is slow per call; columns repeat
  mutable int cache_origin_ = 0;
  std::array<Layer, 3> layers_;
  std::vector<std::uint32_t> rock_;  // repeating cave wall texture, kRockSize x kRockSize
};

}  // namespace px
