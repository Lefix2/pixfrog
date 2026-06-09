// Anti-aliased UI font — generated from a TTF by tools/fontgen.
//
// Each glyph is a fixed kFontCellWidth × kFontHeight cell of 8-bit coverage
// (alpha), row-major. The colour TFT blends fg→bg by this coverage; the mono
// OLED thresholds it at 50 %. The cell carries its own 1px right-hand spacing,
// so the advance equals kFontCellWidth. Geometry here MUST match fontgen.cpp.
//
// Covers printable ASCII 0x20..0x7E (95 glyphs); other chars render as a box.

#pragma once

#include <stdint.h>

namespace pixfrog::ui::detail {

constexpr uint8_t kFontWidth     = 5;  // visible ink columns
constexpr uint8_t kFontCellWidth = 6;  // advance (ink + 1 spacing column)
constexpr uint8_t kFontHeight    = 8;
constexpr char kFontFirstChar    = 0x20;
constexpr char kFontLastChar     = 0x7E;

extern const uint8_t kFontAlpha[][kFontCellWidth * kFontHeight];

// Returns a kFontCellWidth*kFontHeight coverage cell for `c`.
// Out-of-range chars return a filled box.
const uint8_t* font_alpha_for(char c);

}  // namespace pixfrog::ui::detail
