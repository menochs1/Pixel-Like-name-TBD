#include "render/text.hpp"

#include <algorithm>
#include <array>

namespace px {
namespace {

/// One glyph: 5 rows of 3 pixels, '#' = lit. Written as pictures so they are
/// easy to check and edit.
struct Glyph {
  char c;
  const char* rows[kGlyphHeight];
};

constexpr Glyph kGlyphs[] = {
    {'A', {".#.", "#.#", "###", "#.#", "#.#"}}, {'B', {"##.", "#.#", "##.", "#.#", "##."}},
    {'C', {".##", "#..", "#..", "#..", ".##"}}, {'D', {"##.", "#.#", "#.#", "#.#", "##."}},
    {'E', {"###", "#..", "##.", "#..", "###"}}, {'F', {"###", "#..", "##.", "#..", "#.."}},
    {'G', {".##", "#..", "#.#", "#.#", ".##"}}, {'H', {"#.#", "#.#", "###", "#.#", "#.#"}},
    {'I', {"###", ".#.", ".#.", ".#.", "###"}}, {'J', {"..#", "..#", "..#", "#.#", ".#."}},
    {'K', {"#.#", "#.#", "##.", "#.#", "#.#"}}, {'L', {"#..", "#..", "#..", "#..", "###"}},
    {'M', {"#.#", "###", "###", "#.#", "#.#"}}, {'N', {"##.", "#.#", "#.#", "#.#", "#.#"}},
    {'O', {".#.", "#.#", "#.#", "#.#", ".#."}}, {'P', {"##.", "#.#", "##.", "#..", "#.."}},
    {'Q', {".#.", "#.#", "#.#", "##.", ".##"}}, {'R', {"##.", "#.#", "##.", "#.#", "#.#"}},
    {'S', {".##", "#..", ".#.", "..#", "##."}}, {'T', {"###", ".#.", ".#.", ".#.", ".#."}},
    {'U', {"#.#", "#.#", "#.#", "#.#", "###"}}, {'V', {"#.#", "#.#", "#.#", "#.#", ".#."}},
    {'W', {"#.#", "#.#", "###", "###", "#.#"}}, {'X', {"#.#", "#.#", ".#.", "#.#", "#.#"}},
    {'Y', {"#.#", "#.#", ".#.", ".#.", ".#."}}, {'Z', {"###", "..#", ".#.", "#..", "###"}},
    {'0', {"###", "#.#", "#.#", "#.#", "###"}}, {'1', {".#.", "##.", ".#.", ".#.", "###"}},
    {'2', {"##.", "..#", ".#.", "#..", "###"}}, {'3', {"##.", "..#", ".#.", "..#", "##."}},
    {'4', {"#.#", "#.#", "###", "..#", "..#"}}, {'5', {"###", "#..", "##.", "..#", "##."}},
    {'6', {".##", "#..", "###", "#.#", "###"}}, {'7', {"###", "..#", ".#.", ".#.", ".#."}},
    {'8', {"###", "#.#", "###", "#.#", "###"}}, {'9', {"###", "#.#", "###", "..#", "##."}},
    {' ', {"...", "...", "...", "...", "..."}}, {'.', {"...", "...", "...", "...", ".#."}},
    {',', {"...", "...", "...", ".#.", "#.."}}, {':', {"...", ".#.", "...", ".#.", "..."}},
    {';', {"...", ".#.", "...", ".#.", "#.."}}, {'-', {"...", "...", "###", "...", "..."}},
    {'+', {"...", ".#.", "###", ".#.", "..."}}, {'/', {"..#", "..#", ".#.", "#..", "#.."}},
    {'%', {"#.#", "..#", ".#.", "#..", "#.#"}}, {'#', {"#.#", "###", "#.#", "###", "#.#"}},
    {'(', {".#.", "#..", "#..", "#..", ".#."}}, {')', {".#.", "..#", "..#", "..#", ".#."}},
    {'[', {"##.", "#..", "#..", "#..", "##."}}, {']', {".##", "..#", "..#", "..#", ".##"}},
    {'=', {"...", "###", "...", "###", "..."}}, {'_', {"...", "...", "...", "...", "###"}},
    {'>', {"#..", ".#.", "..#", ".#.", "#.."}}, {'<', {"..#", ".#.", "#..", ".#.", "..#"}},
    {'!', {".#.", ".#.", ".#.", "...", ".#."}}, {'?', {"##.", "..#", ".#.", "...", ".#."}},
    {'*', {"...", "#.#", ".#.", "#.#", "..."}}, {'\'', {".#.", ".#.", "...", "...", "..."}},
    {'"', {"#.#", "#.#", "...", "...", "..."}}, {'|', {".#.", ".#.", ".#.", ".#.", ".#."}},
    {'@', {".#.", "#.#", "###", "#..", ".##"}}, {'&', {".#.", "#.#", ".#.", "#.#", ".##"}},
};

constexpr const char* kMissing[kGlyphHeight] = {"###", "#.#", "#.#", "#.#", "###"};

/// Glyph bitmaps by character, each row packed into 3 bits (bit 2 = left).
struct Font {
  std::array<std::array<std::uint8_t, kGlyphHeight>, 128> rows{};
  std::array<bool, 128> present{};
  Font() {
    for (const Glyph& g : kGlyphs) {
      const auto i = static_cast<std::size_t>(g.c);
      for (int r = 0; r < kGlyphHeight; ++r) {
        std::uint8_t bits = 0;
        for (int k = 0; k < kGlyphWidth; ++k) bits = static_cast<std::uint8_t>(bits << 1 | (g.rows[r][k] == '#' ? 1 : 0));
        rows[i][static_cast<std::size_t>(r)] = bits;
      }
      present[i] = true;
    }
  }
};

const Font& font() {
  static const Font f;
  return f;
}

char normalize(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

}  // namespace

bool has_glyph(char c) {
  const auto u = static_cast<unsigned char>(normalize(c));
  return u < 128 && font().present[u];
}

int text_width(std::string_view text, int scale) {
  if (text.empty()) return 0;
  return (static_cast<int>(text.size()) * kGlyphAdvance - (kGlyphAdvance - kGlyphWidth)) * scale;
}

void draw_text(std::uint32_t* pixels, int pitch, int width, int height, int x, int y, std::string_view text,
               std::uint32_t color, int scale) {
  const Font& f = font();
  for (const char raw : text) {
    const auto u = static_cast<unsigned char>(normalize(raw));
    std::array<std::uint8_t, kGlyphHeight> rows{};
    if (u < 128 && f.present[u]) {
      rows = f.rows[u];
    } else {
      for (int r = 0; r < kGlyphHeight; ++r)
        for (int k = 0; k < kGlyphWidth; ++k)
          rows[static_cast<std::size_t>(r)] =
              static_cast<std::uint8_t>(rows[static_cast<std::size_t>(r)] << 1 | (kMissing[r][k] == '#' ? 1 : 0));
    }
    for (int r = 0; r < kGlyphHeight; ++r) {
      for (int k = 0; k < kGlyphWidth; ++k) {
        if (!((rows[static_cast<std::size_t>(r)] >> (kGlyphWidth - 1 - k)) & 1U)) continue;
        for (int sy = 0; sy < scale; ++sy) {
          const int py = y + r * scale + sy;
          if (py < 0 || py >= height) continue;
          for (int sx = 0; sx < scale; ++sx) {
            const int px_ = x + k * scale + sx;
            if (px_ < 0 || px_ >= width) continue;
            pixels[static_cast<std::ptrdiff_t>(py) * pitch + px_] = color;
          }
        }
      }
    }
    x += kGlyphAdvance * scale;
  }
}

void draw_text_panel(std::uint32_t* pixels, int pitch, int width, int height, int x, int y,
                     const std::vector<std::string>& lines, std::uint32_t color) {
  if (lines.empty()) return;
  int w = 0;
  for (const std::string& line : lines) w = std::max(w, text_width(line));
  const int pad = 3;
  const int x0 = std::max(0, x), y0 = std::max(0, y);
  const int x1 = std::min(width, x + w + 2 * pad);
  const int y1 = std::min(height, y + static_cast<int>(lines.size()) * kLineHeight - 1 + 2 * pad);
  // Darken what is behind the box to ~30%, so the text reads on any background.
  for (int py = y0; py < y1; ++py) {
    for (int px_ = x0; px_ < x1; ++px_) {
      std::uint32_t& p = pixels[static_cast<std::ptrdiff_t>(py) * pitch + px_];
      p = 0xFF000000U | (((p >> 16 & 0xFF) * 3 / 10) << 16) | (((p >> 8 & 0xFF) * 3 / 10) << 8) | ((p & 0xFF) * 3 / 10);
    }
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    draw_text(pixels, pitch, width, height, x + pad, y + pad + static_cast<int>(i) * kLineHeight, lines[i],
              i == 0 ? 0xFFFFFFFFU : color);
  }
}

}  // namespace px
