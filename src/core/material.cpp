#include "core/material.hpp"

#include <stdexcept>
#include <utility>

namespace px {

MaterialRegistry::MaterialRegistry() {
  MaterialDef empty;
  empty.name = "empty";
  empty.phase = Phase::Empty;
  empty.density = 1.2f;  // air
  empty.color = {14, 13, 20};
  add(std::move(empty));
}

MaterialId MaterialRegistry::add(MaterialDef def) {
  if (defs_.size() >= kOutOfBounds) {
    throw std::length_error("MaterialRegistry: too many materials");
  }
  if (find(def.name)) {
    throw std::invalid_argument("MaterialRegistry: duplicate material name '" + def.name + "'");
  }
  const auto id = static_cast<MaterialId>(defs_.size());
  phase_.push_back(def.phase);
  density_.push_back(def.density);
  dispersion_.push_back(def.dispersion);
  defs_.push_back(std::move(def));
  return id;
}

std::optional<MaterialId> MaterialRegistry::find(std::string_view name) const {
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    if (defs_[i].name == name) return static_cast<MaterialId>(i);
  }
  return std::nullopt;
}

BaseMaterials register_base_materials(MaterialRegistry& registry) {
  BaseMaterials ids;

  MaterialDef stone;
  stone.name = "stone";
  stone.phase = Phase::Solid;
  stone.density = 2600.0f;
  stone.color = {104, 100, 112};
  stone.color_variation = 14;
  stone.conductivity = 0.3f;
  stone.durability = 200;
  ids.stone = registry.add(std::move(stone));

  MaterialDef sand;
  sand.name = "sand";
  sand.phase = Phase::Powder;
  sand.density = 1600.0f;
  sand.color = {212, 180, 112};
  sand.color_variation = 22;
  sand.durability = 10;
  ids.sand = registry.add(std::move(sand));

  MaterialDef water;
  water.name = "water";
  water.phase = Phase::Liquid;
  water.density = 1000.0f;
  water.color = {48, 104, 196};
  water.color_variation = 10;
  water.dispersion = 5;
  water.conductivity = 0.6f;
  ids.water = registry.add(std::move(water));

  return ids;
}

}  // namespace px
