# fontgen

Rasterises a TTF into the pixfrog UI **alpha font tables** — the anti-aliased
coverage bitmaps consumed by `components/ui/src/canvas_tft.cpp` (blended fg/bg)
and `oled_ssd1306.cpp` (thresholded to 1bpp).

Two cells come out of one run:

- **6×8** (`kFontAlpha`) — base cell, all builds, used at odd text scales.
- **12×16** (`kFontLargeAlpha`) — exactly 2× the base geometry, TFT-only
  (`#ifdef CONFIG_PIXFROG_DISPLAY_TFT` in the generated file). The TFT renderer
  swaps it in for even text scales (scale 2 → large ×1, scale 4 → large ×2), so
  layout maths stay in small-cell units while the glyphs are natively
  rasterised instead of pixel-doubled — much crisper at the dashboard size.

The font is **generated**, not hand-edited. `font_data.cpp` carries a banner
saying so; regenerate it instead of touching it.

## Build & run

```bash
cd tools/fontgen && cmake -B build && cmake --build build
./build/fontgen <font.ttf> ../../components/ui/src/font_data.cpp \
    [px] [baseline] [xpad] [gain] [px_l] [baseline_l] [gain_l]
```

The shipped font comes from **DejaVu Sans Mono** with the canonical metrics
(small cell `px=9 baseline=6 xpad=0 gain=2`, large cell at the defaults
`px_l=19 baseline_l=12 gain_l=1.1`):

```bash
./build/fontgen /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
    ../../components/ui/src/font_data.cpp 9 6 0 2
```

| arg           | meaning                                                            |
|---------------|--------------------------------------------------------------------|
| `px` / `px_l` | nominal pixel height passed to stb (tune so caps fill the cell)    |
| `baseline(_l)`| cell row of the glyph baseline; descenders fall below it           |
| `xpad`        | extra left inset inside the 5px visible glyph box (small cell)     |
| `gain(_l)`    | coverage multiplier; boosts thin stems that cap well below 255     |

Quick metric tuning without a rebuild: pass `preview:SOME TEXT` as the output
path to render both cell sizes as terminal ASCII shading.

Cell geometry (6×8 and 12×16, ASCII 0x20..0x7E) is fixed in `fontgen.cpp` and
must match `components/ui/src/font.h`. After regenerating, rebuild the emulator
to preview: `cd tools/emulator && cmake --build build && ./build/pixfrog_emu`.

`stb_truetype.h` is vendored here (public domain, v1.26).
