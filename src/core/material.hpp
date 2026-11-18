#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px {

using MaterialId = std::uint16_t;
using Temperature = std::int16_t;  // degrees Celsius

/// Id 0 is always "empty" (air). Registered automatically by MaterialRegistry.
inline constexpr MaterialId kEmpty = 0;
/// Returned by World::get for coordinates outside the world. Never stored in a cell.
inline constexpr MaterialId kOutOfBounds = 0xFFFF;
/// Temperature that air relaxes toward and that new cells start at by default.
inline constexpr Temperature kAmbient = 20;

/// Movement rule family. The simulator dispatches on this, never on a specific
/// material, so new materials are added as data only.
enum class Phase : std::uint8_t {
  Empty,   // nothing; anything can move into it
  Solid,   // static, never moves (stone, wood, ice, fire)
  Powder,  // falls, slides diagonally, sinks through lighter fluids (sand)
  Liquid,  // falls, flows sideways up to `dispersion` cells per step (water, oil, lava)
  Gas,     // rises with `rise_chance`, drifts sideways (smoke, steam, flame)
};

struct Color {
  std::uint8_t r = 0, g = 0, b = 0;
};

/// Authoring description of a material. Other materials are referenced by name
/// and resolved by MaterialRegistry::finalize(), so definitions can appear in
/// any order. Probabilities are per simulation step (60 per second).
struct MaterialDef {
  std::string name;
  Phase phase = Phase::Empty;
  float density = 0.0f;              // kg/m^3; denser movers displace lighter fluids/gases
  Color color{};
  std::uint8_t color_variation = 0;  // +/- brightness jitter per pixel
  bool flicker = false;              // visual only: the renderer animates its colour (fire, lava)

  // Movement
  std::uint8_t dispersion = 0;  // liquids/gases: max sideways cells per step
  float rise_chance = 0.0f;     // gases: chance to move up each step

  // Heat
  float conductivity = 0.0f;                       // 0..1, how fast heat flows through it; 0 = perfect insulator
  std::optional<Temperature> spawn_temperature;    // temperature when created (else kept/ambient)
  bool emits_heat = false;                         // holds spawn_temperature (fire, lava)

  // Temperature transitions: above/below a threshold, turn into another
  // material with the given chance per step. Burning is a high transition into
  // fire; boiling, condensing, melting and freezing are the same rule.
  std::optional<Temperature> high_temp;
  std::string high_to;
  float high_chance = 1.0f;
  std::optional<Temperature> low_temp;
  std::string low_to;
  float low_chance = 1.0f;
  /// Lifetime (steps) given to the material this one turns into through a high
  /// transition, e.g. how long burning wood stays on fire. 0 = use the target's own.
  std::uint16_t burn_time = 0;

  // Limited lifetime (fire, flame, smoke). 0 = lives forever.
  std::uint16_t lifetime_min = 0, lifetime_max = 0;
  std::string decay_to;         // what it may leave behind ("" = empty)
  float decay_chance = 1.0f;    // chance to leave decay_to instead of empty

  // Emission into an empty neighbour, mostly upward (fire -> flame).
  std::string emits;
  float emit_chance = 0.0f;

  std::uint16_t durability = 0;  // reserved: digging / explosions (steps 4, 5)
};

/// Hot, resolved per-material properties used by the simulator's inner loop.
/// Probabilities are 16-bit thresholds compared against random bits.
struct MaterialProps {
  Phase phase = Phase::Empty;
  float density = 0.0f;
  std::uint8_t dispersion = 0;
  std::uint16_t rise_chance = 0;
  float conductivity = 0.0f;
  bool has_spawn_temp = false;
  bool emits_heat = false;
  bool flicker = false;
  bool has_reactions = false;
  Temperature spawn_temp = kAmbient;
  bool has_high = false, has_low = false;
  Temperature high_temp = 0, low_temp = 0;
  MaterialId high_to = kEmpty, low_to = kEmpty;
  std::uint16_t high_chance = 0, low_chance = 0;
  std::uint16_t burn_time = 0;
  std::uint16_t lifetime_min = 0, lifetime_max = 0;
  MaterialId decay_to = kEmpty;
  std::uint16_t decay_chance = 0;
  MaterialId emits = kEmpty;
  std::uint16_t emit_chance = 0;
  /// True if a cell of this material at ambient temperature with no lifetime
  /// running can still change state (emits heat, reacts, decays, emits,
  /// or has a transition that triggers at ambient). Flicker is not
  /// simulation state: the renderer animates it. Lets the
  /// simulator skip all state logic for e.g. room-temperature sand and water.
  bool active_at_ambient = false;
};

/// When `a` touches `b`, with `chance` per step, `a` becomes `a_to` and `b`
/// becomes `b_to`. Checked in both orders.
struct Reaction {
  MaterialId a, b, a_to, b_to;
  std::uint16_t chance;
};

/// Owns all material definitions and the reaction table.
class MaterialRegistry {
 public:
  MaterialRegistry();  // registers "empty" (air) as id 0

  MaterialId add(MaterialDef def);
  /// Adds a pair reaction by material names. Call finalize() afterwards.
  /// Only cells of `a` search for partners, so name the rarer or more active
  /// material first (e.g. "lava", "water"): every water cell scanning its
  /// neighbours for lava each step would make all water ~50% more expensive.
  void add_reaction(std::string_view a, std::string_view b, std::string_view a_to, std::string_view b_to,
                    float chance);
  /// Resolves names into ids and builds lookup tables. Must be called after the
  /// last add()/add_reaction() and before simulating. Throws on unknown names.
  void finalize();

  const MaterialDef& def(MaterialId id) const { return defs_[id]; }
  const MaterialProps& props(MaterialId id) const { return props_[id]; }
  Phase phase(MaterialId id) const { return props_[id].phase; }
  float density(MaterialId id) const { return props_[id].density; }
  std::uint8_t dispersion(MaterialId id) const { return props_[id].dispersion; }
  std::size_t size() const { return defs_.size(); }

  std::optional<MaterialId> find(std::string_view name) const;
  MaterialId require(std::string_view name) const;  // throws if missing

  /// Reaction where `a` is the acting cell and `b` its neighbour, or nullptr.
  /// Both orders are stored, so callers never need to swap arguments.
  const Reaction* reaction(MaterialId a, MaterialId b) const {
    const std::int32_t i = reaction_index_[static_cast<std::size_t>(a) * defs_.size() + b];
    return i < 0 ? nullptr : &reactions_[static_cast<std::size_t>(i)];
  }

 private:
  struct PendingReaction {
    std::string a, b, a_to, b_to;
    float chance;
  };
  std::vector<MaterialDef> defs_;
  std::vector<MaterialProps> props_;
  std::vector<PendingReaction> pending_reactions_;
  std::vector<Reaction> reactions_;
  std::vector<std::int32_t> reaction_index_;  // size()^2 dense table, -1 = none
};

/// Ids of the built-in materials.
struct BaseMaterials {
  MaterialId empty = kEmpty;
  MaterialId stone = kEmpty;
  MaterialId sand = kEmpty;
  MaterialId water = kEmpty;
  MaterialId oil = kEmpty;
  MaterialId wood = kEmpty;
  MaterialId fire = kEmpty;
  MaterialId flame = kEmpty;
  MaterialId smoke = kEmpty;
  MaterialId steam = kEmpty;
  MaterialId lava = kEmpty;
  MaterialId ice = kEmpty;
  MaterialId dirt = kEmpty;
  MaterialId grass = kEmpty;
  MaterialId coal = kEmpty;
  MaterialId leaves = kEmpty;
  MaterialId basalt = kEmpty;
};

/// Registers the built-in materials and reactions, finalizes the registry and
/// returns the ids.
BaseMaterials register_base_materials(MaterialRegistry& registry);

}  // namespace px
