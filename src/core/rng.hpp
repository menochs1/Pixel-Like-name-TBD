#pragma once

#include <cstdint>

namespace px {

/// Integer avalanche hash (lowbias32). Cheap and well mixed.
inline std::uint32_t hash32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

/// Stateless random bits for a cell at a given tick. Because the result depends
/// only on (seed, x, y, tick) and not on update order, the simulation stays
/// deterministic even if chunks are later updated in parallel.
inline std::uint32_t cell_hash(std::uint64_t seed, int x, int y, std::uint32_t tick) {
  std::uint32_t h = hash32(static_cast<std::uint32_t>(seed) ^ hash32(static_cast<std::uint32_t>(seed >> 32)));
  h = hash32(h ^ (static_cast<std::uint32_t>(x) * 0x9E3779B1U));
  h = hash32(h ^ (static_cast<std::uint32_t>(y) * 0x85EBCA77U));
  h = hash32(h ^ (tick * 0xC2B2AE3DU));
  return h;
}

}  // namespace px
