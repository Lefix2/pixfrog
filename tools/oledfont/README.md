# oledfont

Emits the **crisp 1bpp 5×7 OLED font** (`components/ui/src/font_oled.cpp`) used
by `oled_ssd1306.cpp`. The mono OLED used to threshold the anti-aliased TFT font
(`font_data.cpp`) at render time, which reads rough at 6×8; this font is drawn
**for the pixel grid** instead, so glyphs stay clean.

The font is **generated**, not hand-edited — the source of truth is the `GLYPHS`
table of ASCII art in `gen.py`. Edit the art, regenerate.

## Build & run

```bash
python3 tools/oledfont/gen.py        # writes components/ui/src/font_oled.cpp
```

Output is column-major: `kFontOled5x7[95][kFontWidth]`, one byte per glyph
column (col 0 = left), bit *r* = row *r* from the top (bit0 = top, rows 0..6).
That matches the SSD1306 page layout, so the renderer copies a column byte
straight into the framebuffer — no per-pixel work. The 6th advance column
(spacing) is added by the renderer, matching the AA font's `kFontCellWidth`.

Covers ASCII 0x20..0x7E. Geometry (`kFontWidth`, `kFontFirstChar`, …) lives in
`components/ui/src/font.h`. `font_oled.cpp` is in `.clang-format-ignore`.

Quick check: render any string from the generated table —

```bash
g++ -std=c++17 -Icomponents/ui/src dump.cpp components/ui/src/font_oled.cpp -o /tmp/d
```
(see the dumper pattern in the git history of this change).
