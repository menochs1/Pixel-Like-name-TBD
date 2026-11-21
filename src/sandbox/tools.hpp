#pragma once

#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/world.hpp"

namespace px {

/// A brush that pulls temperatures toward `target`, like holding something
/// very hot or very cold against the world. Each application closes
/// `strength` of the gap over the inner half of the brush, fading to nothing
/// at the rim, so it works on any material at any temperature and never
/// overshoots the target.
struct ThermalBrush {
  int target = kAmbient;  // degrees C
  float strength = 0.0f;  // share of the gap closed per application, inner half
};

/// Heats quickly: rock at the centre passes its 1500 C melting point in about
/// a quarter of a second.
inline constexpr ThermalBrush kHeatBrush{3000, 0.05f};
/// Cools hard enough to freeze water, harden lava (below 600 C) and put out
/// fire (below 300 C) in one application over the inner half of the brush.
inline constexpr ThermalBrush kCoolBrush{-100, 0.8f};

/// Applies `brush` once to the disc of `radius` around (cx, cy). Air is
/// affected too (it then drifts back to ambient). Call it once per simulation
/// step while the tool is held, so the effect does not depend on frame rate.
/// Wraps on a wrapping world; changed cells are woken.
void apply_thermal_brush(World& world, int cx, int cy, int radius, const ThermalBrush& brush);

/// Everything the inspector shows about one cell, read without changing it.
struct CellInfo {
  bool in_world = false;
  int x = 0, y = 0;            // wrapped
  MaterialId material = kEmpty;
  MaterialId base = kEmpty;    // the element it is (a variant of)
  bool derived = false;        // a variant with its own properties
  Phase phase = Phase::Empty;
  int temperature = kAmbient;
  int health_percent = 100;
  int durability = 0;          // the material's
  float density = 0.0f;
  float conductivity = 0.0f;
  bool has_high = false;       // has a high transition (burns, melts, boils)
  int transition_temp = 0;     // ignition / melting / boiling point
  std::string transition_to;   // what it turns into above that
  float transition_chance = 0.0f;  // per step, once above
  bool has_low = false;
  int low_temp = 0;            // freezing / hardening point
  std::string low_to;
  int life = 0;                // steps left (solids and gases); 0 = none
  int fall_speed = 0;          // powders and liquids; negative = settle count
  int flow = 0;                // liquids: -1, 0, +1
  bool active = false;         // its chunk is scheduled to simulate
};

CellInfo inspect(const World& world, int x, int y);

/// The inspector panel as lines of text, for drawing or printing.
std::vector<std::string> describe(const World& world, const CellInfo& info);

const char* phase_name(Phase phase);

}  // namespace px
