#include "sandbox/tools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace px {

void apply_thermal_brush(World& world, int cx, int cy, int radius, const ThermalBrush& brush) {
  if (radius < 0 || brush.strength <= 0.0f) return;
  const float rim = static_cast<float>(radius) + 1.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy > radius * radius) continue;
      const int x = cx + dx, y = cy + dy;
      if (!world.in_bounds(x, y)) continue;
      // Full strength over the inner half of the brush, fading linearly to
      // zero just past the rim.
      const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      const float k = brush.strength * std::clamp((rim - d) / (rim * 0.5f), 0.0f, 1.0f);
      const int t = world.temperature(x, y);
      const int change = static_cast<int>(std::lround(static_cast<float>(brush.target - t) * k));
      if (change != 0) world.set_temperature(x, y, t + change);
    }
  }
}

const char* phase_name(Phase phase) {
  switch (phase) {
    case Phase::Empty: return "air";
    case Phase::Solid: return "solid";
    case Phase::Powder: return "powder";
    case Phase::Liquid: return "liquid";
    case Phase::Gas: return "gas";
  }
  return "?";
}

CellInfo inspect(const World& world, int x, int y) {
  CellInfo info;
  if (!world.in_bounds(x, y)) return info;
  x = world.wrap_x(x);
  const MaterialRegistry& reg = world.materials();
  const Chunk& c = world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  const MaterialId m = c.material[i];
  const MaterialProps& p = reg.props(m);
  info.in_world = true;
  info.x = x;
  info.y = y;
  info.material = m;
  info.base = p.base;
  info.derived = p.base != m;
  info.phase = p.phase;
  info.temperature = c.temperature(i);
  info.health_percent = (c.health[i] * 100 + 127) / 255;
  info.durability = p.durability;
  info.density = p.density;
  info.conductivity = p.conductivity;
  if (p.has_high) {
    info.has_high = true;
    info.transition_temp = p.high_temp;
    info.transition_to = reg.def(p.high_to).name;
    info.transition_chance = static_cast<float>(p.high_chance) / 65535.0f;
  }
  if (p.has_low) {
    info.has_low = true;
    info.low_temp = p.low_temp;
    info.low_to = reg.def(p.low_to).name;
  }
  if (p.uses_motion) {
    info.fall_speed = c.fall_speed(i);
    info.flow = c.flow(i);
  } else {
    info.life = c.life(i);
  }
  const int lx = x & kChunkMask, ly = y & kChunkMask;
  const DirtyRect& r = c.next_dirty;
  info.active = lx >= r.min_x && lx <= r.max_x && ly >= r.min_y && ly <= r.max_y;  // false for an empty rect
  return info;
}

namespace {

#if defined(__GNUC__)
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
std::string format(const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return buf;
}

}  // namespace

std::vector<std::string> describe(const World& world, const CellInfo& info) {
  std::vector<std::string> lines;
  if (!info.in_world) {
    lines.push_back("outside the world");
    return lines;
  }
  const MaterialRegistry& reg = world.materials();
  const std::string& name = reg.def(info.material).name;
  if (info.derived) {
    lines.push_back(format("%s  (derived %s)", name.c_str(), reg.def(info.base).name.c_str()));
  } else {
    lines.push_back(format("%s  (%s)", name.c_str(), phase_name(info.phase)));
  }
  lines.push_back(format("at %d,%d  %s", info.x, info.y, info.active ? "simulating" : "asleep"));
  lines.push_back(format("temp %d C", info.temperature));
  if (info.material == kEmpty) return lines;
  if (info.derived) lines.push_back(format("phase %s", phase_name(info.phase)));
  lines.push_back(format("density %.0f  conduct %.2f", static_cast<double>(info.density),
                         static_cast<double>(info.conductivity)));
  if (info.durability > 0) {
    lines.push_back(format("health %d%% of %d", info.health_percent, info.durability));
  } else {
    lines.push_back("health: breaks at a touch");
  }
  if (info.has_high) {
    lines.push_back(format("above %d C: %s (%.1f%%/step)", info.transition_temp, info.transition_to.c_str(),
                           static_cast<double>(info.transition_chance) * 100.0));
  }
  if (info.has_low) lines.push_back(format("below %d C: %s", info.low_temp, info.low_to.c_str()));
  if (info.life > 0) lines.push_back(format("life %d steps", info.life));
  if (info.phase == Phase::Powder || info.phase == Phase::Liquid) {
    if (info.fall_speed < 0) lines.push_back(format("settling %d", -info.fall_speed));
    else if (info.fall_speed > 0) lines.push_back(format("falling, speed %d", info.fall_speed));
    if (info.flow != 0) lines.push_back(format("flowing %s", info.flow > 0 ? "right" : "left"));
  }
  return lines;
}

}  // namespace px
