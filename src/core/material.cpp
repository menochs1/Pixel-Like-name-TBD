#include "core/material.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <climits>
#include <string>
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
  if (derived_ > 0) {  // derived ids are about to become invalid anyway (see finalize)
    defs_.resize(defs_.size() - derived_);
    props_.resize(props_.size() - derived_);
    variants_.clear();
    records_.clear();
    derived_ = 0;
  }
  if (defs_.size() >= kMaxMaterials) throw std::length_error("MaterialRegistry: too many materials");
  if (find(def.name)) throw std::invalid_argument("MaterialRegistry: duplicate material name '" + def.name + "'");
  const auto id = static_cast<MaterialId>(defs_.size());
  defs_.push_back(std::move(def));
  return id;
}

void MaterialRegistry::add_reaction(std::string_view a, std::string_view b, std::string_view a_to,
                                    std::string_view b_to, float chance) {
  ReactionDef r;
  r.a = a;
  r.b = b;
  r.a_to = a_to;
  r.b_to = b_to;
  r.chance = chance;
  add_reaction(std::move(r));
}

void MaterialRegistry::add_reaction(ReactionDef def) { pending_reactions_.push_back(std::move(def)); }

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

namespace {

/// Materials that need state updates even at ambient temperature with no lifetime running.
bool active_at_ambient(const MaterialProps& p) {
  return p.emits_heat || p.has_reactions || p.lifetime_max > 0 || p.emit_chance > 0 ||
         (p.has_spawn_temp && p.spawn_temp != kAmbient) || (p.has_high && kAmbient >= p.high_temp) ||
         (p.has_low && kAmbient <= p.low_temp);
}

}  // namespace

MaterialProps MaterialRegistry::resolve(const MaterialDef& d) const {
  MaterialProps p;
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
  p.uses_motion = d.phase == Phase::Powder || d.phase == Phase::Liquid;
  p.durability = d.durability;
  p.passable = d.passable;
  return p;
}

void MaterialRegistry::finalize() {
  if (derived_ > 0) {
    defs_.resize(defs_.size() - derived_);
    variants_.clear();
    records_.clear();
    derived_ = 0;
  }
  authored_count_ = defs_.size();
  props_.clear();
  props_.reserve(kMaxMaterials);  // never reallocates afterwards: see kMaxMaterials
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    props_.push_back(resolve(defs_[i]));
    props_.back().base = static_cast<MaterialId>(i);
    if (props_.back().uses_motion && props_.back().lifetime_max > 0) {
      throw std::invalid_argument("MaterialRegistry: '" + defs_[i].name +
                                  "' is a powder or liquid with a lifetime; only solids and gases can expire");
    }
  }
  // A burn_time is a lifetime given to the transition target, so the same rule applies to it.
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    if (props_[i].has_high && props_[i].burn_time > 0 && props_[props_[i].high_to].uses_motion) {
      throw std::invalid_argument("MaterialRegistry: '" + defs_[i].name +
                                  "' has a burn_time but turns into a powder or liquid, which cannot expire");
    }
  }

  reactions_.clear();
  derivations_.clear();
  reaction_index_.assign(authored_count_ * authored_count_, -1);
  auto put = [&](MaterialId a, MaterialId b, MaterialId a_to, MaterialId b_to, std::uint16_t chance,
                 std::int16_t heat, const Derivation* ad, const Derivation* bd, bool swapped) {
    reaction_index_[static_cast<std::size_t>(a) * authored_count_ + b] = static_cast<std::int32_t>(reactions_.size());
    reactions_.push_back({a, b, a_to, b_to, chance, heat, ad, bd, swapped});
  };
  auto keep = [&](const std::optional<Derivation>& d) -> const Derivation* {
    if (!d) return nullptr;
    derivations_.push_back(std::make_unique<Derivation>(*d));
    return derivations_.back().get();
  };
  for (const ReactionDef& r : pending_reactions_) {
    const MaterialId a = require(r.a), b = require(r.b);
    const MaterialId a_to = require(r.a_to), b_to = require(r.b_to);
    const std::uint16_t chance = to_chance(r.chance);
    const auto heat = static_cast<std::int16_t>(std::clamp(r.heat, -4000, 4000));
    const Derivation* ad = keep(r.a_derive);
    const Derivation* bd = keep(r.b_derive);
    put(a, b, a_to, b_to, chance, heat, ad, bd, false);
    if (a != b) put(b, a, b_to, a_to, chance, heat, bd, ad, true);
    // Only the first-named material looks for partners; one side checking is
    // enough to find every contact. Name the rarer / more active material
    // first (lava before water) so common materials stay on the fast path.
    props_[a].has_reactions = true;
  }
  for (MaterialProps& p : props_) p.active_at_ambient = active_at_ambient(p);
}

// ------------------------------------------------------------------ derived materials

std::size_t MaterialRegistry::VariantHash::operator()(const VariantRecord& k) const {
  std::uint64_t h = 1469598103934665603ULL;
  auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
  mix(k.base);
  mix(static_cast<std::uint32_t>(k.density));
  mix(static_cast<std::uint32_t>(k.conductivity));
  mix(static_cast<std::uint32_t>(k.ignition));
  mix(static_cast<std::uint32_t>(k.flammability));
  mix((static_cast<std::uint32_t>(k.r) << 16) | (static_cast<std::uint32_t>(k.g) << 8) | k.b);
  return static_cast<std::size_t>(h);
}

namespace {

float apply(const Derivation::Rule& rule, float product, float a, float b) {
  switch (rule.op) {
    case Derivation::Op::Product: return product;
    case Derivation::Op::A: return a;
    case Derivation::Op::B: return b;
    case Derivation::Op::Mean: return (a + b) * 0.5f;
    case Derivation::Op::Min: return std::min(a, b);
    case Derivation::Op::Max: return std::max(a, b);
    case Derivation::Op::Mix: return rule.weight * a + (1.0f - rule.weight) * b;
  }
  return product;
}

// Derived values are rounded to these steps before interning, so long chains
// of reactions converge on a bounded set of variants instead of minting a new
// one for every tiny difference.
constexpr float kDensityStep = 10.0f;        // kg/m^3
constexpr float kConductivityStep = 0.01f;
constexpr float kIgnitionStep = 5.0f;        // degrees
constexpr float kFlammabilityStep = 0.001f;  // chance per step

std::int32_t quantize(float v, float step) { return static_cast<std::int32_t>(std::lround(v / step)); }

}  // namespace

MaterialId MaterialRegistry::derive(MaterialId product, const Derivation& rule, MaterialId a, MaterialId b) {
  product = props_[product].base;
  const MaterialDef& pd = defs_[product];
  const MaterialDef& da = defs_[a];
  const MaterialDef& db = defs_[b];

  const float density = apply(rule.density, pd.density, da.density, db.density);
  const float conductivity = std::clamp(apply(rule.conductivity, pd.conductivity, da.conductivity, db.conductivity), 0.0f, 1.0f);
  auto channel = [&](std::uint8_t p, std::uint8_t x, std::uint8_t y) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(apply(rule.color, p, x, y)), 0L, 255L));
  };
  const Color color{channel(pd.color.r, da.color.r, db.color.r), channel(pd.color.g, da.color.g, db.color.g),
                    channel(pd.color.b, da.color.b, db.color.b)};
  // Ignition and flammability only mean something if the product element has a
  // high transition. A parent without one counts as non-flammable (chance 0)
  // and lends no ignition temperature (the other parent's is used).
  std::optional<Temperature> ignition = pd.high_temp;
  float flammability = pd.high_temp ? pd.high_chance : 0.0f;
  if (pd.high_temp) {
    const float pt = *pd.high_temp;
    const float at = da.high_temp ? *da.high_temp : (db.high_temp ? *db.high_temp : pt);
    const float bt = db.high_temp ? *db.high_temp : at;
    ignition = static_cast<Temperature>(std::lround(apply(rule.ignition, pt, at, bt)));
    flammability = std::clamp(apply(rule.flammability, pd.high_chance, da.high_temp ? da.high_chance : 0.0f,
                                    db.high_temp ? db.high_chance : 0.0f),
                              0.0f, 1.0f);
  }

  VariantRecord rec;
  rec.base = product;
  rec.density = quantize(density, kDensityStep);
  rec.conductivity = quantize(conductivity, kConductivityStep);
  rec.ignition = ignition ? quantize(*ignition, kIgnitionStep) : INT32_MIN;
  rec.flammability = quantize(flammability, kFlammabilityStep);
  rec.r = color.r;
  rec.g = color.g;
  rec.b = color.b;
  return variant(rec);
}

VariantRecord MaterialRegistry::record_of(MaterialId authored) const {
  const MaterialDef& d = defs_[authored];
  VariantRecord rec;
  rec.base = authored;
  rec.density = quantize(d.density, kDensityStep);
  rec.conductivity = quantize(d.conductivity, kConductivityStep);
  rec.ignition = d.high_temp ? quantize(*d.high_temp, kIgnitionStep) : INT32_MIN;
  rec.flammability = quantize(d.high_temp ? d.high_chance : 0.0f, kFlammabilityStep);
  rec.r = d.color.r;
  rec.g = d.color.g;
  rec.b = d.color.b;
  return rec;
}

MaterialId MaterialRegistry::variant(const VariantRecord& record) {
  if (record.base >= authored_count_) return kEmpty;  // not an authored element: invalid record
  const MaterialId product = record.base;
  if (record == record_of(product)) return product;  // no different from the element itself
  if (auto it = variants_.find(record); it != variants_.end()) return it->second;
  if (props_.size() >= kMaxMaterials) return product;  // full: fall back to the plain element

  const MaterialDef& pd = defs_[product];
  MaterialDef v = pd;
  v.name = pd.name + "#" + std::to_string(derived_ + 1);
  v.density = static_cast<float>(record.density) * kDensityStep;
  v.conductivity = std::clamp(static_cast<float>(record.conductivity) * kConductivityStep, 0.0f, 1.0f);
  v.color = {record.r, record.g, record.b};
  if (pd.high_temp && record.ignition != INT32_MIN) {
    v.high_temp = static_cast<Temperature>(record.ignition * static_cast<std::int32_t>(kIgnitionStep));
    v.high_chance = std::clamp(static_cast<float>(record.flammability) * kFlammabilityStep, 0.0f, 1.0f);
  }
  const auto id = static_cast<MaterialId>(defs_.size());
  MaterialProps p = resolve(v);
  p.base = product;
  p.has_reactions = props_[product].has_reactions;
  p.active_at_ambient = active_at_ambient(p);
  defs_.push_back(std::move(v));
  props_.push_back(p);  // within the reserved capacity: references stay valid
  variants_.emplace(record, id);
  records_.push_back(record);
  ++derived_;
  return id;
}

std::optional<VariantRecord> MaterialRegistry::variant_record(MaterialId id) const {
  if (id < authored_count_ || id >= defs_.size()) return std::nullopt;
  return records_[id - authored_count_];
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
    d.high_temp = 1500;  // melts (only a heating brush gets rock this hot)
    d.high_to = "lava";
    d.high_chance = 0.05f;
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
    d.low_temp = 300;  // cooled this far, it goes out
    d.low_to = "smoke";
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
    d.low_temp = 600;  // cooled this far, it hardens back into rock
    d.low_to = "stone";
    d.low_chance = 0.5f;
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
    d.passable = true;  // scenery: the player walks through foliage
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

  {
    // Oil-soaked sand. The defaults here are only a fallback: tar made by the
    // oil + sand reaction below takes its density, colour, ignition point and
    // flammability from the oil and sand it came from.
    MaterialDef d;
    d.name = "tar";
    d.phase = Phase::Liquid;
    d.density = 1200;
    d.color = {58, 44, 30};
    d.color_variation = 10;
    d.dispersion = 1;  // barely flows
    d.conductivity = 0.25f;
    d.high_temp = 220;
    d.high_to = "fire";
    d.high_chance = 0.05f;
    d.burn_time = 400;  // burns long and slow
    d.durability = 30;
    ids.tar = add(d);
  }

  {
    // Wood that grew as a tree: burns like wood, but the player walks past it
    // (trees are scenery). Painted wood stays solid for building.
    MaterialDef d;
    d.name = "trunk";
    d.phase = Phase::Solid;
    d.density = 700;
    d.color = {110, 72, 42};
    d.color_variation = 14;
    d.conductivity = 0.15f;
    d.high_temp = 260;
    d.high_to = "fire";
    d.high_chance = 0.04f;
    d.burn_time = 200;
    d.durability = 40;
    d.passable = true;
    ids.trunk = add(d);
  }
  {
    // Where the player stands, for the length of one simulation step
    // (Player::occupy/release): solid, so sand piles on it and water flows
    // around it, and insulating, so it does not soak up heat. Never left in
    // the world between steps.
    MaterialDef d;
    d.name = "body";
    d.phase = Phase::Solid;
    d.density = 1000;
    d.color = {255, 0, 255};  // never drawn: a visible magenta if it ever leaks
    d.conductivity = 0.0f;
    d.internal = true;
    ids.body = add(d);
  }

  // Pair reactions for contact effects that temperature alone does not give.
  registry.add_reaction("lava", "water", "stone", "steam", 0.35f);
  registry.add_reaction("fire", "water", "smoke", "steam", 0.5f);    // doused
  registry.add_reaction("flame", "water", "empty", "water", 1.0f);   // snuffed
  registry.add_reaction("lava", "ice", "stone", "water", 0.3f);

  // Derived product: oil soaks into sand and both become tar whose properties
  // come from its parents. Heavier than oil (so it sinks through it), darker
  // than sand, and only half as eager to burn as oil, since sand does not burn.
  {
    ReactionDef r;
    r.a = "oil";  // the rarer partner scans for contacts
    r.b = "sand";
    r.a_to = "tar";
    r.b_to = "tar";
    r.chance = 0.02f;  // soaks in over a few seconds
    Derivation d;
    d.density = {Derivation::Op::Mean};
    d.conductivity = {Derivation::Op::Mean};
    d.color = {Derivation::Op::Mix, 0.6f};          // mostly oil-dark
    d.ignition = {Derivation::Op::A};               // oil's ignition point
    d.flammability = {Derivation::Op::Mix, 0.5f};   // diluted by the sand
    r.a_derive = d;
    r.b_derive = d;
    registry.add_reaction(std::move(r));
  }

  registry.finalize();
  return ids;
}

}  // namespace px
