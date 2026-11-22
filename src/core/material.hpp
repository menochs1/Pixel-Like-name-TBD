#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px {

using MaterialId = std::uint16_t;
using Temperature = std::int16_t;  // degrees Celsius

/// Id 0 is always "empty" (air). Registered automatically by MaterialRegistry.
inline constexpr MaterialId kEmpty = 0;
/// Returned by World::get for coordinates outside the world. Never stored in a cell.
inline constexpr MaterialId kOutOfBounds = 0xFFFF;
/// Upper bound on materials, authored plus derived. Storage for this many is
/// reserved up front so that deriving a material mid-step never moves the
/// property table (the simulator holds references into it).
inline constexpr std::size_t kMaxMaterials = 16384;
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

  // Limited lifetime (fire, flame, smoke). 0 = lives forever. Only solids and
  // gases may have one: powders and liquids use that per-cell storage for
  // their motion (see Chunk::motion), and finalize() rejects the combination.
  std::uint16_t lifetime_min = 0, lifetime_max = 0;
  std::string decay_to;         // what it may leave behind ("" = empty)
  float decay_chance = 1.0f;    // chance to leave decay_to instead of empty

  // Emission into an empty neighbour, mostly upward (fire -> flame).
  std::string emits;
  float emit_chance = 0.0f;

  // Crowding: when at least `crowd_count` of its 8 neighbours are the same
  // element, turn into `crowd_to` with `crowd_chance` per step (e.g. a dense
  // gas condensing). 0 = no crowding rule.
  std::uint8_t crowd_count = 0;
  std::string crowd_to;
  float crowd_chance = 0.0f;
  float lone_chance = 0.0f;  // the same change when not crowded

  // Weather.
  bool windblown = false;      // gases drift with the wind; liquids and powders drift while falling (rain, snow)
  bool joins_weather = false;  // vapour: rising above World::Weather::vapour_y, it leaves the world and
                               // becomes moisture in the weather overhead (see core/weather.hpp)

  std::uint16_t durability = 0;  // damage a cell takes before it breaks (World::damage); 0 = breaks at once

  /// Used by the engine itself (the player's body) and not offered as a
  /// paintable material.
  bool internal = false;
  /// The player walks through it although it is solid to the simulation
  /// (foliage: tree trunks and leaves are scenery, not walls).
  bool passable = false;
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
  /// Powders and liquids: the cell's motion field holds fall speed and flow
  /// direction. Everything else: it holds the remaining lifetime.
  bool uses_motion = false;
  /// The authored element this material is, or a variant of. Equal to its own
  /// id for authored materials. Reactions are looked up by base element, so a
  /// variant of tar reacts like tar.
  MaterialId base = kEmpty;
  std::uint16_t durability = 0;
  bool passable = false;  // the player walks through it
  std::uint8_t crowd_count = 0;
  MaterialId crowd_to = kEmpty;
  std::uint16_t crowd_chance = 0;
  std::uint16_t lone_chance = 0;
  bool windblown = false;
  bool joins_weather = false;
};

/// How a reaction computes a product's properties from its two parents, the
/// reacting cells (which may themselves be derived). Each property has its own
/// rule; the product's element supplies phase, behaviour and anything without
/// a rule. The result is interned as a variant of the product element, so equal
/// results share one material id.
struct Derivation {
  enum class Op : std::uint8_t {
    Product,  // keep the product element's own value
    A,        // copy from the first-named reactant
    B,        // copy from the second-named reactant
    Mean,     // average of both
    Min,
    Max,
    Mix,      // weight * A + (1 - weight) * B
  };
  struct Rule {
    Op op = Op::Product;
    float weight = 0.5f;  // Mix only
  };
  Rule density;       // kg/m^3: sinking and floating
  Rule conductivity;  // heat flow
  Rule color;         // per channel
  Rule ignition;      // high-transition temperature (ignition / melting point)
  Rule flammability;  // high-transition chance per step
};

/// A pair reaction as authored: when `a` touches `b`, with `chance` per step,
/// `a` becomes `a_to` and `b` becomes `b_to`. Either product may be derived
/// from both reactants, and the reaction may release (or absorb) heat.
struct ReactionDef {
  std::string a, b, a_to, b_to;
  float chance = 1.0f;
  int heat = 0;  // degrees added to both products (negative absorbs heat)
  std::optional<Derivation> a_derive, b_derive;
};

/// A resolved reaction, as seen from the acting cell `a` (stored in both orders).
struct Reaction {
  MaterialId a, b, a_to, b_to;  // base elements
  std::uint16_t chance;
  std::int16_t heat;
  const Derivation* a_derive;   // nullptr: product is a_to as authored
  const Derivation* b_derive;
  bool swapped;                 // stored with the authored roles of a and b exchanged
};

/// The properties that make a derived material differ from its element, as
/// rounded integers (density in 10 kg/m^3 steps, conductivity in 0.01,
/// ignition in 5 C, flammability in 0.001 per step). Equal records are the same
/// material. This is also how derived materials are saved and restored.
struct VariantRecord {
  MaterialId base = kEmpty;
  std::int32_t density = 0, conductivity = 0;
  std::int32_t ignition = INT32_MIN;  // INT32_MIN: the element has no high transition
  std::int32_t flammability = 0;
  std::uint8_t r = 0, g = 0, b = 0;
  bool operator==(const VariantRecord&) const = default;
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
  /// Adds a reaction that may derive its products and release heat.
  void add_reaction(ReactionDef def);
  /// Resolves names into ids and builds lookup tables. Must be called after the
  /// last add()/add_reaction() and before simulating. Throws on unknown names.
  /// Discards any derived materials (their ids would no longer be valid).
  void finalize();

  /// The variant of `product` whose properties `rule` computes from parents
  /// `a` and `b`. Returns an existing material when the (rounded) result
  /// matches one, so repeated reactions reuse ids. Deterministic: the same
  /// sequence of calls gives the same ids. When the table is full, returns
  /// `product` itself.
  MaterialId derive(MaterialId product, const Derivation& rule, MaterialId a, MaterialId b);
  /// Number of materials created by derive().
  std::size_t derived_count() const { return derived_; }
  /// The variant described by `record` (creating it if needed), or its base
  /// element if the record matches the element itself or the table is full.
  /// derive() goes through this; loading a save uses it to restore variants.
  MaterialId variant(const VariantRecord& record);
  /// The record of a derived material, or nullopt for an authored one.
  std::optional<VariantRecord> variant_record(MaterialId id) const;
  /// Materials defined by add() (ids below this); the rest are derived.
  std::size_t authored_count() const { return authored_count_; }

  const MaterialDef& def(MaterialId id) const { return defs_[id]; }
  const MaterialProps& props(MaterialId id) const { return props_[id]; }
  Phase phase(MaterialId id) const { return props_[id].phase; }
  float density(MaterialId id) const { return props_[id].density; }
  std::uint8_t dispersion(MaterialId id) const { return props_[id].dispersion; }
  std::size_t size() const { return defs_.size(); }

  std::optional<MaterialId> find(std::string_view name) const;
  MaterialId require(std::string_view name) const;  // throws if missing

  /// Reaction where `a` is the acting cell and `b` its neighbour, or nullptr.
  /// Both orders are stored, so callers never need to swap arguments. Looked
  /// up by base element: variants react like the element they derive from.
  const Reaction* reaction(MaterialId a, MaterialId b) const {
    const std::size_t i = static_cast<std::size_t>(props_[a].base) * authored_count_ + props_[b].base;
    const std::int32_t r = reaction_index_[i];
    return r < 0 ? nullptr : &reactions_[static_cast<std::size_t>(r)];
  }

 private:
  struct VariantHash {
    std::size_t operator()(const VariantRecord& k) const;
  };
  VariantRecord record_of(MaterialId authored) const;  // an element's own values, rounded
  MaterialProps resolve(const MaterialDef& d) const;

  std::vector<MaterialDef> defs_;
  std::vector<MaterialProps> props_;
  std::vector<ReactionDef> pending_reactions_;
  std::vector<Reaction> reactions_;
  std::vector<std::unique_ptr<Derivation>> derivations_;  // stable addresses for Reaction
  std::vector<std::int32_t> reaction_index_;  // authored_count_^2 dense table, -1 = none
  std::size_t authored_count_ = 0;  // materials known at the last finalize()
  std::size_t derived_ = 0;         // materials created by derive() since then (they follow the authored ones)
  std::unordered_map<VariantRecord, MaterialId, VariantHash> variants_;
  std::vector<VariantRecord> records_;  // per derived material, in id order
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
  MaterialId tar = kEmpty;
  MaterialId body = kEmpty;
  MaterialId trunk = kEmpty;
  MaterialId snow = kEmpty;
};

/// Registers the built-in materials and reactions, finalizes the registry and
/// returns the ids.
BaseMaterials register_base_materials(MaterialRegistry& registry);

}  // namespace px
