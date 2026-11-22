#include "core/weather.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/rng.hpp"

namespace px {

WeatherStats update_weather(World& world, const ChunkRegion& region) {
  WeatherStats stats;
  const Weather& w = world.weather();
  const int areas = world.weather_areas();
  if (areas == 0) return stats;
  const int area_width = std::max(1, w.region_width);

  // 1. Moisture drifts downwind (with the wind up where the weather is).
  const float wind = world.wind_at(w.vapour_y > 0 ? w.vapour_y : 0);
  const float share = std::min(0.5f, std::fabs(wind) / static_cast<float>(area_width));
  if (share > 0.0f && areas > 1) {
    const int dir = wind > 0.0f ? 1 : -1;
    std::vector<float> moved(static_cast<std::size_t>(areas), 0.0f);
    for (int a = 0; a < areas; ++a) {
      int to = a + dir;
      if (to < 0 || to >= areas) {
        if (!world.wraps_x()) continue;  // walled world: it piles up at the edge
        to = (to + areas) % areas;
      }
      const float out = world.moisture(a) * share;
      world.moisture(a) -= out;
      moved[static_cast<std::size_t>(to)] += out;
    }
    for (int a = 0; a < areas; ++a) world.moisture(a) += moved[static_cast<std::size_t>(a)];
  }

  // 2. Humidity builds; areas start and stop raining; rain uses moisture up.
  std::vector<float> rate(static_cast<std::size_t>(areas), 0.0f);  // drops per step in each raining area
  for (int a = 0; a < areas; ++a) {
    float& m = world.moisture(a);
    m += w.humidity_rate;
    if (!world.raining(a) && m >= w.rain_start) world.set_raining(a, true);
    if (world.raining(a) && m <= w.rain_stop) world.set_raining(a, false);
    if (!world.raining(a)) continue;
    ++stats.raining_areas;
    // Heavier when very wet (after a big boil-off), lighter as it dries out.
    const float r = w.rain_per_step * std::clamp(m / std::max(1.0f, w.rain_start), 0.3f, 2.0f);
    rate[static_cast<std::size_t>(a)] = r;
    m = std::max(0.0f, m - r);
  }
  if (stats.raining_areas == 0) return stats;

  // 3. Drops fall in from the top of the simulated box, over raining areas.
  const int top = std::max(0, region.cy0) << kChunkShift;
  if (w.ground_y > 0 && top >= w.ground_y - 40) return stats;  // looking at caves: no rain in here
  const MaterialRegistry& reg = world.materials();
  const MaterialId water = reg.require("water");
  const MaterialId snow = reg.find("snow").value_or(water);
  const MaterialId drop = world.ambient_at(top) < 0 ? snow : water;
  int x0 = region.cx0 << kChunkShift, x1 = ((region.cx1 + 1) << kChunkShift) - 1;
  if (!world.wraps_x()) {
    x0 = std::max(x0, 0);
    x1 = std::min(x1, world.width() - 1);
  } else if (x1 - x0 + 1 > world.width()) {
    x1 = x0 + world.width() - 1;
  }
  for (int x = x0; x <= x1; ++x) {
    const float r = rate[static_cast<std::size_t>(world.weather_area(x))];
    if (r <= 0.0f) continue;
    // Each column of a raining area gets a drop with chance rate / width.
    const float chance = r / static_cast<float>(area_width);
    const std::uint32_t roll = cell_hash(world.seed() ^ 0x5241494EU, world.wrap_x(x), top, world.tick());
    if (static_cast<float>(roll & 0xFFFFFF) / 16777216.0f >= chance) continue;
    if (world.get(x, top) != kEmpty) continue;
    world.set(x, top, drop);
    ++stats.drops;
  }
  return stats;
}

}  // namespace px
