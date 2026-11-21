#include "save/chunk_codec.hpp"

#include <array>

#include "core/rng.hpp"
#include "save/bytes.hpp"

namespace px {
namespace {

constexpr std::uint32_t kChunkMagic = 0x4B435850;  // "PXCK"
constexpr std::uint16_t kChunkVersion = 1;

/// Runs of equal values: (run length as varint, value).
template <typename T, typename Get>
void write_runs(ByteWriter& w, Get get) {
  int i = 0;
  while (i < kChunkArea) {
    const T v = get(i);
    int j = i + 1;
    while (j < kChunkArea && get(j) == v) ++j;
    w.varint(static_cast<std::uint32_t>(j - i));
    if constexpr (sizeof(T) == 1) w.u8(static_cast<std::uint8_t>(v));
    else w.u16(static_cast<std::uint16_t>(v));
    i = j;
  }
}

template <typename T, typename Put>
bool read_runs(ByteReader& r, Put put) {
  int i = 0;
  while (i < kChunkArea) {
    const std::uint32_t n = r.varint();
    const T v = sizeof(T) == 1 ? static_cast<T>(r.u8()) : static_cast<T>(r.u16());
    if (!r.ok() || n == 0 || n > static_cast<std::uint32_t>(kChunkArea - i)) return false;
    for (std::uint32_t k = 0; k < n; ++k) put(i++, v);
  }
  return true;
}

/// The shade a cell placed at this spot gets (World::set, world generation).
std::array<std::uint8_t, kChunkArea> expected_shades(int cx, int cy, std::uint64_t seed) {
  std::array<std::uint8_t, kChunkArea> s{};
  for (int ly = 0; ly < kChunkSize; ++ly)
    for (int lx = 0; lx < kChunkSize; ++lx)
      s[static_cast<std::size_t>(Chunk::index(lx, ly))] = static_cast<std::uint8_t>(
          cell_hash(seed, (cx << kChunkShift) + lx, (cy << kChunkShift) + ly, 0) & 0x0F);
  return s;
}

bool fail(std::string* error, const char* why) {
  if (error) *error = why;
  return false;
}

}  // namespace

std::vector<std::uint8_t> encode_chunk(const Chunk& c, int cx, int cy, std::uint64_t seed) {
  ByteWriter w;
  w.u32(kChunkMagic);
  w.u16(kChunkVersion);
  w.i32(cx);
  w.i32(cy);
  const DirtyRect& d = c.next_dirty;
  w.u8(d.empty() ? 0 : 1);
  if (!d.empty()) {
    w.u8(static_cast<std::uint8_t>(d.min_x));
    w.u8(static_cast<std::uint8_t>(d.min_y));
    w.u8(static_cast<std::uint8_t>(d.max_x));
    w.u8(static_cast<std::uint8_t>(d.max_y));
  }
  const auto expected = expected_shades(cx, cy, seed);
  write_runs<std::uint16_t>(w, [&](int i) { return c.material[static_cast<std::size_t>(i)]; });
  write_runs<std::uint16_t>(w, [&](int i) { return static_cast<std::uint16_t>(c.thermal[static_cast<std::size_t>(i)] & 0x0FFF); });
  // Shades: relative to the position-derived shade (terrain, painted cells) or
  // as they are (chunks created empty have shade 0), whichever is smaller.
  ByteWriter relative, plain;
  write_runs<std::uint8_t>(relative, [&](int i) {
    return static_cast<std::uint8_t>(c.shade(i) ^ expected[static_cast<std::size_t>(i)]);
  });
  write_runs<std::uint8_t>(plain, [&](int i) { return c.shade(i); });
  const bool use_relative = relative.bytes().size() <= plain.bytes().size();
  w.u8(use_relative ? 1 : 0);
  const std::vector<std::uint8_t>& shades = use_relative ? relative.bytes() : plain.bytes();
  w.bytes().insert(w.bytes().end(), shades.begin(), shades.end());
  write_runs<std::uint16_t>(w, [&](int i) { return c.motion[static_cast<std::size_t>(i)]; });
  write_runs<std::uint8_t>(w, [&](int i) { return c.health[static_cast<std::size_t>(i)]; });
  std::vector<std::uint8_t>& out = w.bytes();
  const std::uint32_t sum = checksum(out.data(), out.size());
  w.u32(sum);
  return std::move(w.bytes());
}

bool decode_chunk(const std::uint8_t* data, std::size_t size, int cx, int cy, std::uint64_t seed,
                  const std::vector<std::uint16_t>* remap, std::size_t material_count, Chunk& out,
                  std::string* error) {
  out = Chunk{};
  if (size < 4 + 4) return fail(error, "chunk data too short");
  std::uint32_t stored = 0;
  for (int k = 0; k < 4; ++k) stored |= static_cast<std::uint32_t>(data[size - 4 + static_cast<std::size_t>(k)]) << (8 * k);
  if (checksum(data, size - 4) != stored) return fail(error, "chunk data damaged (checksum mismatch)");

  ByteReader r(data, size - 4);
  if (r.u32() != kChunkMagic) return fail(error, "not chunk data");
  if (r.u16() != kChunkVersion) return fail(error, "unsupported chunk format version");
  if (r.i32() != cx || r.i32() != cy) return fail(error, "chunk data belongs to another position");
  if (r.u8()) {
    DirtyRect d;
    d.min_x = r.u8();
    d.min_y = r.u8();
    d.max_x = r.u8();
    d.max_y = r.u8();
    if (d.max_x >= kChunkSize || d.max_y >= kChunkSize || d.min_x > d.max_x || d.min_y > d.max_y) {
      return fail(error, "chunk data has a bad pending-work box");
    }
    out.next_dirty = d;
  }
  bool ids_ok = true;
  const bool ok =
      read_runs<std::uint16_t>(r,
                               [&](int i, std::uint16_t v) {
                                 if (remap) v = v < remap->size() ? (*remap)[v] : kOutOfBounds;
                                 if (v >= material_count) ids_ok = false;
                                 out.material[static_cast<std::size_t>(i)] = v;
                               }) &&
      read_runs<std::uint16_t>(r, [&](int i, std::uint16_t v) { out.thermal[static_cast<std::size_t>(i)] = v & 0x0FFF; });
  if (!ok) {
    out = Chunk{};
    return fail(error, "chunk data truncated");
  }
  if (!ids_ok) {
    out = Chunk{};
    return fail(error, "chunk data uses an unknown material");
  }
  const bool relative = r.u8() != 0;
  const auto expected = relative ? expected_shades(cx, cy, seed) : std::array<std::uint8_t, kChunkArea>{};
  const bool rest =
      read_runs<std::uint8_t>(r,
                              [&](int i, std::uint8_t v) {
                                out.thermal[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(
                                    out.thermal[static_cast<std::size_t>(i)] |
                                    (((v ^ expected[static_cast<std::size_t>(i)]) & 0x0F) << 12));
                              }) &&
      read_runs<std::uint16_t>(r, [&](int i, std::uint16_t v) { out.motion[static_cast<std::size_t>(i)] = v; }) &&
      read_runs<std::uint8_t>(r, [&](int i, std::uint8_t v) { out.health[static_cast<std::size_t>(i)] = v; });
  if (!rest || r.remaining() != 0) {
    out = Chunk{};
    return fail(error, "chunk data truncated or has trailing bytes");
  }
  out.touched = false;
  return true;
}

}  // namespace px
