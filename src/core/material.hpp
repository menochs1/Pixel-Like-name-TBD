#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px {

using MaterialId = std::uint16_t;

/// Id 0 is always "empty" (air). Registered automatically by MaterialRegistry.
inline constexpr MaterialId kEmpty = 0;
/// Returned by World::get for coordinates outside the world. Never stored in a cell.
inline constexpr MaterialId kOutOfBounds = 0xFFFF;

/// Movement rule family. The simulator dispatches on this, never on a specific
/// material, so new materials are added as data only.
enum class Phase : std::uint8_t {
  Empty,   // nothing; anything can move into it
  Solid,   // static, never moves (stone, wood, metal)
  Powder,  // falls, slides diagonally, sinks through lighter fluids (sand)
  Liquid,  // falls, flows sideways up to `dispersion` cells per step (water)
  Gas,     // reserved for roadmap step 2 (smoke, steam)
};

struct Color {
  std::uint8_t r = 0, g = 0, b = 0;
};

/// Authoring description of a material. Fields marked "reserved" are stored now
/// so the data format is stable, and get used by later roadmap steps.
struct MaterialDef {
  std::string name;
  Phase phase = Phase::Empty;
  float density = 0.0f;            // kg/m^3; denser movers displace lighter fluids
  Color color{};
  std::uint8_t color_variation = 0;  // +/- brightness jitter per pixel
  std::uint8_t dispersion = 0;       // liquids: max sideways cells per step
  float flammability = 0.0f;         // reserved: fire (step 2)
  float conductivity = 0.0f;         // reserved: heat / electricity (steps 2, 6)
  std::uint16_t durability = 0;      // reserved: digging / explosions (steps 4, 5)
};

/// Owns all material definitions. Hot per-cell properties are also kept in
/// parallel arrays (structure of arrays) so the simulator's inner loop touches
/// a few bytes per lookup instead of a whole MaterialDef with a std::string.
class MaterialRegistry {
 public:
  MaterialRegistry();  // registers "empty" as id 0

  MaterialId add(MaterialDef def);

  const MaterialDef& def(MaterialId id) const { return defs_[id]; }
  Phase phase(MaterialId id) const { return phase_[id]; }
  float density(MaterialId id) const { return density_[id]; }
  std::uint8_t dispersion(MaterialId id) const { return dispersion_[id]; }
  std::size_t size() const { return defs_.size(); }

  std::optional<MaterialId> find(std::string_view name) const;

 private:
  std::vector<MaterialDef> defs_;
  std::vector<Phase> phase_;
  std::vector<float> density_;
  std::vector<std::uint8_t> dispersion_;
};

/// Ids of the built-in roadmap-step-1 materials.
struct BaseMaterials {
  MaterialId empty = kEmpty;
  MaterialId stone = kEmpty;
  MaterialId sand = kEmpty;
  MaterialId water = kEmpty;
};

/// Registers stone, sand and water into `registry` and returns their ids.
BaseMaterials register_base_materials(MaterialRegistry& registry);

}  // namespace px
