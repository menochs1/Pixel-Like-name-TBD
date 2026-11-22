#pragma once

#include "core/simulator.hpp"
#include "core/world.hpp"

namespace px {

struct WeatherStats {
  int raining_areas = 0;  // weather areas where it is raining now
  int drops = 0;          // raindrops / snowflakes placed this step
};

/// One step of the regional weather (settings in World::Weather):
///
///  - Every weather area gains moisture by itself (humidity_rate), and gains
///    a cell's worth for every cell of vapour (steam) that rises out of sight
///    above vapour_y over it (the simulator does that part): boil a lake and
///    the rain comes to that area.
///  - The wind high up carries moisture into the neighbouring area downwind,
///    so wet weather drifts.
///  - An area starts raining once its moisture reaches rain_start and stops
///    at rain_stop. While raining it loses rain_per_step (more when very wet)
///    cells of water a step, and that rain falls: drops are placed along the
///    top row of `region` (the simulated box, which extends above what the
///    player sees), so they fall into view from above. Where the air there is
///    below freezing (World::ambient_at), snow falls instead.
///
/// Rain over areas outside `region` still uses up moisture (it rains where
/// nobody is looking). No rain is placed when the top of `region` is
/// underground. Deterministic: depends on the seed, the step and `region`.
/// Call once per simulation step, before step().
WeatherStats update_weather(World& world, const ChunkRegion& region);

}  // namespace px
