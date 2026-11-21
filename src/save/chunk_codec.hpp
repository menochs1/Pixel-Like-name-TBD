#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/chunk.hpp"

namespace px {

/// Serialises one chunk: every cell's material, temperature, colour shade,
/// motion/lifetime and health, plus its pending work (next_dirty), so a chunk
/// written out and read back continues exactly as if it had never left.
///
/// Each field is stored as its own run-length encoded stream: rock and air
/// come out as a handful of runs. The colour shade is stored relative to the
/// shade a cell placed at that spot gets (a hash of its position), so the
/// shade of cells that never moved costs almost nothing, or as-is when that
/// is smaller (chunks created empty). A checksum rejects
/// truncated or damaged data. A typical chunk is a few hundred bytes; the
/// in-memory chunk is ~28 KiB.
///
/// `seed`, `cx` and `cy` must match between encoding and decoding (they
/// define the expected shades); decode checks the coordinates.
std::vector<std::uint8_t> encode_chunk(const Chunk& chunk, int cx, int cy, std::uint64_t seed);

/// Decodes into `out`, which is reset first. Material ids are passed through
/// `remap` (index: id as saved, value: id now) when given, and must be below
/// `material_count`. On failure returns false, leaves `out` empty and
/// describes the problem in `error`.
bool decode_chunk(const std::uint8_t* data, std::size_t size, int cx, int cy, std::uint64_t seed,
                  const std::vector<std::uint16_t>* remap, std::size_t material_count, Chunk& out,
                  std::string* error = nullptr);

}  // namespace px
