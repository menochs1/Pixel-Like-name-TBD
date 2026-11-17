#pragma once

#include "core/material.hpp"
#include "core/world.hpp"

namespace px {

/// A small hand-shaped playground: rolling stone ground, a water basin, a sand
/// dune on a ledge and a few floating stone platforms. Deterministic for a given
/// world seed. Real seeded cave/biome generation is roadmap step 3.
void build_demo_scene(World& world, const BaseMaterials& ids);

/// Stress scene for benchmarking: a band of falling sand and water over the
/// whole width of the world, above a floor with obstacles.
void build_rain_scene(World& world, const BaseMaterials& ids);

}  // namespace px
