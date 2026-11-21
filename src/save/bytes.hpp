#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace px {

/// Little-endian binary writer used by the save format.
class ByteWriter {
 public:
  void u8(std::uint8_t v) { out_.push_back(v); }
  void u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v));
    u8(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v));
    u16(static_cast<std::uint16_t>(v >> 16));
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v));
    u32(static_cast<std::uint32_t>(v >> 32));
  }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void f32(float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    u32(bits);
  }
  /// Unsigned LEB128: small numbers take one byte.
  void varint(std::uint32_t v) {
    while (v >= 0x80) {
      u8(static_cast<std::uint8_t>(v | 0x80));
      v >>= 7;
    }
    u8(static_cast<std::uint8_t>(v));
  }
  void str(const std::string& s) {
    varint(static_cast<std::uint32_t>(s.size()));
    out_.insert(out_.end(), s.begin(), s.end());
  }
  std::vector<std::uint8_t>& bytes() { return out_; }

 private:
  std::vector<std::uint8_t> out_;
};

/// Reader matching ByteWriter. Never reads past the end: once it runs out,
/// ok() is false and every further read returns zero.
class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}
  std::uint8_t u8() {
    if (p_ >= end_) {
      ok_ = false;
      return 0;
    }
    return *p_++;
  }
  std::uint16_t u16() {
    const std::uint16_t lo = u8();
    return static_cast<std::uint16_t>(lo | (u8() << 8));
  }
  std::uint32_t u32() {
    const std::uint32_t lo = u16();
    return lo | (static_cast<std::uint32_t>(u16()) << 16);
  }
  std::uint64_t u64() {
    const std::uint64_t lo = u32();
    return lo | (static_cast<std::uint64_t>(u32()) << 32);
  }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  float f32() {
    const std::uint32_t bits = u32();
    float v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
  }
  std::uint32_t varint() {
    std::uint32_t v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      const std::uint8_t b = u8();
      v |= static_cast<std::uint32_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    ok_ = false;  // too long
    return 0;
  }
  std::string str() {
    const std::uint32_t n = varint();
    if (n > static_cast<std::size_t>(end_ - p_)) {
      ok_ = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
  }
  bool ok() const { return ok_; }
  std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
  const std::uint8_t* position() const { return p_; }

 private:
  const std::uint8_t* p_;
  const std::uint8_t* end_;
  bool ok_ = true;
};

/// FNV-1a: a cheap checksum that catches truncated or damaged files.
inline std::uint32_t checksum(const std::uint8_t* data, std::size_t size) {
  std::uint32_t h = 2166136261U;
  for (std::size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 16777619U;
  return h;
}

}  // namespace px
