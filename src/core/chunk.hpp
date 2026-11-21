#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "core/material.hpp"

namespace px {

inline constexpr int kChunkShift = 6;
inline constexpr int kChunkSize = 1 << kChunkShift;  // 64x64 cells
inline constexpr int kChunkMask = kChunkSize - 1;
inline constexpr int kChunkArea = kChunkSize * kChunkSize;

/// Inclusive bounding box of cells that need updating, in chunk-local coords.
struct DirtyRect {
  int min_x = kChunkSize, min_y = kChunkSize, max_x = -1, max_y = -1;

  bool empty() const { return max_x < min_x; }
  void reset() { *this = DirtyRect{}; }
  void include(int x, int y) {
    min_x = std::min(min_x, x);
    min_y = std::min(min_y, y);
    max_x = std::max(max_x, x);
    max_y = std::max(max_y, y);
  }
  void include_rect(int x0, int y0, int x1, int y1) {
    min_x = std::min(min_x, x0);
    min_y = std::min(min_y, y0);
    max_x = std::max(max_x, x1);
    max_y = std::max(max_y, y1);
  }
  int area() const { return empty() ? 0 : (max_x - min_x + 1) * (max_y - min_y + 1); }
};

/// A 64x64 block of cells stored as a structure of arrays. Each array is
/// indexed by (local_y << kChunkShift) | local_x.
///
/// Memory: 7 bytes per cell plus one bit, packed by what each field really
/// needs (~28.5 KiB per chunk; a 10-million-cell world is ~68 MiB):
///
///   material  16 bits  material id (room for many generated materials)
///   thermal   16 bits  temperature + 273 in the low 12 bits (-273..3822 C),
///                      colour shade 0..15 in the high 4
///   motion    16 bits  powders and liquids: fall speed / settle count (signed,
///                      low 8 bits) and flow direction (bits 8-9);
///                      everything else: remaining lifetime in steps (0 = none).
///                      A cell never needs both, so they share the field.
///   health     8 bits  share of the material's durability left (255 = intact)
///   updated    1 bit   moved or updated during the current step (see below)
///
/// Properties that are fixed for a cell (density, conductivity, colour,
/// ignition point, flammability) are not stored per cell: a cell whose
/// properties differ from its element holds the id of a derived material
/// (MaterialRegistry::derive) that has them.
///
/// Use the accessors rather than the raw arrays, so the packing stays in one place.
struct Chunk {
  static constexpr int kTempBias = 273;
  static constexpr int kMinTemp = -kTempBias;
  static constexpr int kMaxTemp = 0x0FFF - kTempBias;  // 3822

  std::array<MaterialId, kChunkArea> material{};
  std::array<std::uint16_t, kChunkArea> thermal{};
  std::array<std::uint16_t, kChunkArea> motion{};
  std::array<std::uint8_t, kChunkArea> health{};

  /// One bit per cell: updated during step `updated_tick`. Bits from an older
  /// step are stale and cleared on first use (see was_updated), so chunks the
  /// step never touches cost nothing. Replaces a per-cell clock byte.
  std::array<std::uint64_t, kChunkArea / 64> updated_bits{};
  std::uint32_t updated_tick = 0;

  DirtyRect dirty;       // being processed this step (can grow mid-step)
  DirtyRect next_dirty;  // collected this step, processed next step

  /// Changed since it was generated or loaded: any cell write, wake-up or
  /// consumed pending work sets it. An untouched chunk need not be saved: it can
  /// be generated again (or, in a world without a generator, it is empty).
  bool touched = false;

  static constexpr std::uint8_t kFullHealth = 255;

  Chunk() {
    thermal.fill(static_cast<std::uint16_t>(kAmbient + kTempBias));
    health.fill(kFullHealth);
  }

  static constexpr int index(int local_x, int local_y) { return (local_y << kChunkShift) | local_x; }

  // ---- temperature and shade (thermal)
  Temperature temperature(int i) const { return static_cast<Temperature>((thermal[i] & 0x0FFF) - kTempBias); }
  /// Clamped to [kMinTemp, kMaxTemp].
  void set_temperature(int i, int t) {
    touched = true;
    t = std::clamp(t, kMinTemp, kMaxTemp);
    thermal[i] = static_cast<std::uint16_t>((thermal[i] & 0xF000) | (t + kTempBias));
  }
  std::uint8_t shade(int i) const { return static_cast<std::uint8_t>(thermal[i] >> 12); }
  void set_shade(int i, unsigned s) {
    touched = true;
    thermal[i] = static_cast<std::uint16_t>((thermal[i] & 0x0FFF) | ((s & 0x0F) << 12));
  }

  // ---- motion: powders and liquids
  /// Fall speed (>= 0), or for a resting liquid -(settle count).
  int fall_speed(int i) const { return static_cast<std::int8_t>(motion[i] & 0xFF); }
  void set_fall_speed(int i, int v) {
    touched = true;
    motion[i] = static_cast<std::uint16_t>((motion[i] & 0xFF00) | static_cast<std::uint8_t>(static_cast<std::int8_t>(v)));
  }
  /// Liquid flow direction: -1, 0 or +1.
  int flow(int i) const {
    const int f = (motion[i] >> 8) & 3;
    return f == 3 ? -1 : f;
  }
  void set_flow(int i, int d) {
    touched = true;
    motion[i] = static_cast<std::uint16_t>((motion[i] & 0x00FF) | (static_cast<unsigned>(d & 3) << 8));
  }

  // ---- motion: everything else
  std::uint16_t life(int i) const { return motion[i]; }
  void set_life(int i, std::uint16_t steps) {
    touched = true;
    motion[i] = steps;
  }

  // ---- per-step update bit
  bool was_updated(int i, std::uint32_t tick) {
    if (updated_tick != tick) return false;
    return (updated_bits[static_cast<std::size_t>(i >> 6)] >> (i & 63)) & 1U;
  }
  void mark_updated(int i, std::uint32_t tick) {
    touched = true;  // every move and transform marks its cells
    if (updated_tick != tick) {
      updated_bits.fill(0);
      updated_tick = tick;
    }
    updated_bits[static_cast<std::size_t>(i >> 6)] |= std::uint64_t{1} << (i & 63);
  }
  void clear_updated(int i) { updated_bits[static_cast<std::size_t>(i >> 6)] &= ~(std::uint64_t{1} << (i & 63)); }
};

}  // namespace px
