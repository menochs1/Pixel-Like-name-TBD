#include "core/material.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace px {
namespace {

std::uint16_t to_chance(float p) {
  return static_cast<std::uint16_t>(std::lround(std::clamp(p, 0.0f, 1.0f) * 65535.0f));
}

}  // namespace

MaterialRegistry::MaterialRegistry() {
  MaterialDef air;
  air.name = "empty";
  air.phase = Phase::Empty;
  air.density = 1.2f;
  air.color = {14, 13, 20};
  air.conductivity = 0.08f;
  add(std::move(air));
  finalize();
}

MaterialId MaterialRegistry::add(MaterialDef def) {
  if (defs_.size() >= kOutOfBounds) throw std::length_error("MaterialRegistry: too many materials");
  if (find(def.name)) throw std::invalid_argument("MaterialRegistry: duplicate material name '" + def.name + "'");
  const auto id = static_cast<MaterialId>(defs_.size());
  defs_.push_back(std::move(def));
  return id;
}

void MaterialRegistry::add_reaction(std::string_view a, std::string_view b, std::string_view a_to,
                                    std::string_view b_to, float chance) {
  pending_reactions_.push_back({std::string(a), std::string(b), std::string(a_to), std::string(b_to), chance});
}

std::optional<MaterialId> MaterialRegistry::find(std::string_view name) const {
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    if (defs_[i].name == name) return static_cast<MaterialId>(i);
  }
  return std::nullopt;
}

MaterialId MaterialRegistry::require(std::string_view name) const {
  if (name.empty()) return kEmpty;
  if (auto id = find(name)) return *id;
  throw std::invalid_argument("MaterialRegistry: unknown material '" + std::string(name) + "'");
}

void MaterialRegistry::finalize() {
  props_.assign(defs_.size(), MaterialProps{});
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    const MaterialDef& d = defs_[i];
    MaterialProps& p = props_[i];
    p.phase = d.phase;
    p.density = d.density;
    p.dispersion = d.dispersion;
    p.rise_chance = to_chance(d.rise_chance);
    p.conductivity = std::clamp(d.conductivity, 0.0f, 1.0f);
    p.has_spawn_temp = d.spawn_temperature.has_value();
    p.spawn_temp = d.spawn_temperature.value_or(kAmbient);
    p.emits_heat = d.emits_heat && p.has_spawn_temp;
    p.flicker = d.flicker;
    if (d.high_temp) {
      p.has_high = true;
      p.high_temp = *d.high_temp;
      p.high_to = require(d.high_to);
      p.high_chance = to_chance(d.high_chance);
    }
    if (d.low_temp) {
      p.has_low = true;
      p.low_temp = *d.low_temp;
      p.low_to = require(d.low_to);
      p.low_chance = to_chance(d.low_chance);
    }
    p.burn_time = d.burn_time;
    p.lifetime_min = d.lifetime_min;
    p.lifetime_max = std::max(d.lifetime_min, d.lifetime_max);
    p.decay_to = require(d.decay_to);
    p.decay_chance = to_chance(d.decay_chance);
    p.emits = require(d.emits);
    p.emit_chance = to_chance(d.emit_chance);
  }
  // After reactions are known (has_reactions), flag materials that need state
  // updates even at ambient temperature.
  auto flag_active = [&]() {
    for (MaterialProps& p : props_) {
      p.active_at_ambient = p.emits_heat || p.has_reactions || p.lifetime_max > 0 || p.emit_chance > 0 ||
                            (p.has_spawn_temp && p.spawn_temp != kAmbient) ||
                            (p.has_high && kAmbient >= p.high_temp) || (p.has_low && kAmbient <= p.low_temp);
    }
  };

  reactions_.clear();
  reaction_index_.assign(defs_.size() * defs_.size(), -1);
  auto put = [&](MaterialId a, MaterialId b, MaterialId a_to, MaterialId b_to, std::uint16_t chance) {
    reaction_index_[static_cast<std::size_t>(a) * defs_.size() + b] = static_cast<std::int32_t>(reactions_.size());
    reactions_.push_back({a, b, a_to, b_to, chance});
  };
  for (const PendingReaction& r : pending_reactions_) {
    const MaterialId a = require(r.a), b = require(r.b);
    const MaterialId a_to = require(r.a_to), b_to = require(r.b_to);
    const std::uint16_t chance = to_chance(r.chance);
    put(a, b, a_to, b_to, chance);
    if (a != b) put(b, a, b_to, a_to, chance);
    // Only the first-named material looks for partners; one side checking is
    // enough to find every contact. Name the rarer / more active material
    // first (lava before water) so common materials stay on the fast path.
    props_[a].has_reactions = true;
  }
  flag_active();
}

BaseMaterials register_base_materials(MaterialRegistry& registry) {
  auto add = [&](MaterialDef d) { return registry.add(std::move(d)); };
  BaseMaterials ids;

  {
    MaterialDef d;
    d.name = "stone";
    d.phase = Phase::Solid;
    d.density = 2600;
    d.color = {104, 100, 112};
    d.color_variation = 14;
    d.conductivity = 0.35f;
    d.durability = 200;
    ids.stone = add(d);
  }
  {
    MaterialDef d;
    d.name = "sand";
    d.phase = Phase::Powder;
    d.density = 1600;
    d.color = {212, 180, 112};
    d.color_variation = 22;
    d.conductivity = 0.25f;
    d.durability = 10;
    ids.sand = add(d);
  }
  {
    MaterialDef d;
    d.name = "water";
    d.phase = Phase::Liquid;
    d.density = 1000;
    d.color = {48, 104, 196};
    d.color_variation = 10;
    d.dispersion = 8;
    d.conductivity = 0.6f;
    d.high_temp = 100;
    d.high_to = "steam";
    d.high_chance = 0.15f;
    d.low_temp = -2;
    d.low_to = "ice";
    d.low_chance = 0.05f;
    ids.water = add(d);
  }
  {
    MaterialDef d;
    d.name = "oil";
    d.phase = Phase::Liquid;
    d.density = 850;  // floats on water
    d.color = {62, 46, 34};
    d.color_variation = 8;
    d.dispersion = 6;
    d.conductivity = 0.3f;
    d.high_temp = 200;  // ignition point
    d.high_to = "fire";
    d.high_chance = 0.12f;
    d.burn_time = 150;
    ids.oil = add(d);
  }
  {
    MaterialDef d;
    d.name = "wood";
    d.phase = Phase::Solid;
    d.density = 700;
    d.color = {120, 78, 44};
    d.color_variation = 14;
    d.conductivity = 0.15f;
    d.high_temp = 260;
    d.high_to = "fire";
    d.high_chance = 0.04f;
    d.burn_time = 200;
    d.durability = 40;
    ids.wood = add(d);
  }
  {
    // Burning matter: stays in place, holds its heat, throws flames upward.
    MaterialDef d;
    d.name = "fire";
    d.phase = Phase::Solid;
    d.density = 500;
    d.color = {236, 104, 32};
    d.color_variation = 40;
    d.flicker = true;
    d.conductivity = 0.9f;
    d.spawn_temperature = 900;
    d.emits_heat = true;
    d.lifetime_min = 90;  // a painted fire; burning fuel uses its own burn_time
    d.lifetime_max = 160;
    d.decay_to = "smoke";
    d.decay_chance = 0.35f;
    d.emits = "flame";
    d.emit_chance = 0.35f;
    ids.fire = add(d);
  }
  {
    // Short-lived tongue of flame rising off burning matter.
    MaterialDef d;
    d.name = "flame";
    d.phase = Phase::Gas;
    d.density = 0.3f;
    d.color = {252, 196, 64};
    d.color_variation = 40;
    d.flicker = true;
    d.rise_chance = 0.9f;
    d.dispersion = 1;
    d.conductivity = 0.9f;
    d.spawn_temperature = 700;
    d.emits_heat = true;
    d.lifetime_min = 5;
    d.lifetime_max = 16;
    d.decay_to = "smoke";
    d.decay_chance = 0.12f;
    ids.flame = add(d);
  }
  {
    MaterialDef d;
    d.name = "smoke";
    d.phase = Phase::Gas;
    d.density = 0.9f;
    d.color = {70, 68, 74};
    d.color_variation = 12;
    d.rise_chance = 0.7f;
    d.dispersion = 2;
    d.conductivity = 0.1f;
    d.lifetime_min = 90;
    d.lifetime_max = 240;
    ids.smoke = add(d);
  }
  {
    MaterialDef d;
    d.name = "steam";
    d.phase = Phase::Gas;
    d.density = 0.6f;
    d.color = {176, 190, 206};
    d.color_variation = 14;
    d.rise_chance = 0.85f;
    d.dispersion = 2;
    d.conductivity = 0.35f;
    d.spawn_temperature = 120;
    d.low_temp = 90;  // condenses back into water
    d.low_to = "water";
    d.low_chance = 0.01f;
    ids.steam = add(d);
  }
  {
    MaterialDef d;
    d.name = "lava";
    d.phase = Phase::Liquid;
    d.density = 3100;
    d.color = {232, 84, 24};
    d.color_variation = 26;
    d.flicker = true;
    d.dispersion = 2;  // viscous
    d.conductivity = 0.5f;
    d.spawn_temperature = 1200;
    d.emits_heat = true;
    ids.lava = add(d);
  }
  {
    MaterialDef d;
    d.name = "ice";
    d.phase = Phase::Solid;
    d.density = 917;
    d.color = {168, 210, 236};
    d.color_variation = 10;
    d.conductivity = 0.55f;
    d.spawn_temperature = -30;
    d.high_temp = 1;
    d.high_to = "water";
    d.high_chance = 0.004f;  // a block lasts several seconds once thawed
    ids.ice = add(d);
  }

  // Terrain materials used by world generation.
  {
    MaterialDef d;
    d.name = "dirt";
    d.phase = Phase::Solid;
    d.density = 1500;
    d.color = {112, 78, 52};
    d.color_variation = 14;
    d.conductivity = 0.25f;
    d.durability = 60;
    ids.dirt = add(d);
  }
  {
    MaterialDef d;
    d.name = "grass";
    d.phase = Phase::Solid;
    d.density = 1100;
    d.color = {76, 142, 56};
    d.color_variation = 18;
    d.conductivity = 0.2f;
    d.high_temp = 200;
    d.high_to = "fire";
    d.high_chance = 0.08f;
    d.burn_time = 40;  // a quick grass fire
    d.durability = 20;
    ids.grass = add(d);
  }
  {
    MaterialDef d;
    d.name = "coal";
    d.phase = Phase::Solid;
    d.density = 1350;
    d.color = {38, 36, 40};
    d.color_variation = 8;
    d.conductivity = 0.3f;
    d.high_temp = 350;
    d.high_to = "fire";
    d.high_chance = 0.02f;
    d.burn_time = 900;  // hard to light, smoulders for ~15 s
    d.durability = 120;
    ids.coal = add(d);
  }
  {
    MaterialDef d;
    d.name = "leaves";
    d.phase = Phase::Solid;
    d.density = 400;
    d.color = {58, 118, 48};
    d.color_variation = 24;
    d.conductivity = 0.1f;
    d.high_temp = 180;
    d.high_to = "fire";
    d.high_chance = 0.15f;
    d.burn_time = 30;
    d.durability = 5;
    ids.leaves = add(d);
  }

  {
    // Cooled lava rock. A perfect insulator (conductivity 0): a shell of it
    // keeps a buried lava chamber hot inside and the rock around it cold, so
    // neither costs anything until someone digs in.
    MaterialDef d;
    d.name = "basalt";
    d.phase = Phase::Solid;
    d.density = 2900;
    d.color = {58, 52, 58};
    d.color_variation = 10;
    d.conductivity = 0.0f;
    d.durability = 300;
    ids.basalt = add(d);
  }

  // Pair reactions for contact effects that temperature alone does not give.
  registry.add_reaction("lava", "water", "stone", "steam", 0.35f);
  registry.add_reaction("fire", "water", "smoke", "steam", 0.5f);    // doused
  registry.add_reaction("flame", "water", "empty", "water", 1.0f);   // snuffed
  registry.add_reaction("lava", "ice", "stone", "water", 0.3f);

  registry.finalize();
  return ids;
}

}  // namespace px
