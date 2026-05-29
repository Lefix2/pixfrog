// 5×7 bitmap font for the SSD1306 OLED renderer.
//
// Column-major encoding: each glyph is 5 bytes, each byte is one column,
// bit 0 = topmost pixel, bit 6 = bottom of the 7-pixel-high glyph, bit 7
// reserved as inter-line padding (always 0). One blank column is added
// between glyphs by the renderer, so effective cell = 6×8.
//
// Covers printable ASCII 0x20..0x7E (95 glyphs). Any non-printable input
// renders as a filled box via font_glyph_for().

#pragma once

#include <stdint.h>

namespace pixfrog::ui::detail {

constexpr uint8_t kFontWidth      = 5;
constexpr uint8_t kFontCellWidth  = 6;   // glyph + 1 trailing blank column
constexpr uint8_t kFontHeight     = 8;
constexpr char    kFontFirstChar  = 0x20;
constexpr char    kFontLastChar   = 0x7E;

extern const uint8_t kFont5x8[][kFontWidth];

// Returns a 5-byte glyph for `c`. Out-of-range chars render as a filled box.
const uint8_t* font_glyph_for(char c);

}  // namespace pixfrog::ui::detail
