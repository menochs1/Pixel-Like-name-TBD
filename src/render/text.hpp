#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace px {

/// A tiny built-in 3x5 pixel font for on-screen readouts (no font files, no
/// dependencies). Letters are drawn in capitals; characters without a glyph
/// draw as a small box. Each character advances 4 pixels; lines are 6 apart.
inline constexpr int kGlyphWidth = 3;
inline constexpr int kGlyphHeight = 5;
inline constexpr int kGlyphAdvance = 4;
inline constexpr int kLineHeight = 6;

/// True if `c` has a real glyph (letters of either case, digits, common punctuation).
bool has_glyph(char c);

/// Width in pixels of `text` drawn at `scale` (no trailing gap).
int text_width(std::string_view text, int scale = 1);

/// Draws `text` with its top-left at (x, y) into an ARGB buffer of `width` x
/// `height` pixels with row stride `pitch`. Clipped to the buffer.
void draw_text(std::uint32_t* pixels, int pitch, int width, int height, int x, int y, std::string_view text,
               std::uint32_t color, int scale = 1);

/// Draws lines of text on a darkened box at (x, y): the inspector panel.
void draw_text_panel(std::uint32_t* pixels, int pitch, int width, int height, int x, int y,
                     const std::vector<std::string>& lines, std::uint32_t color = 0xFFE8E8F0U);

}  // namespace px
