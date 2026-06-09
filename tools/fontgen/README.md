# fontgen

Rasterises a TTF into the pixfrog UI **alpha font table** — the anti-aliased
coverage bitmap consumed by `components/ui/src/canvas_tft.cpp` (blended fg/bg)
and `oled_ssd1306.cpp` (thresholded to 1bpp).

The font is **generated**, not hand-edited. `font_data.cpp` carries a banner
saying so; regenerate it instead of touching it.

## Build & run

```bash
cd tools/fontgen && cmake -B build && cmake --build build
./build/fontgen <font.ttf> ../../components/ui/src/font_data.cpp [px] [baseline] [xpad]
```

Current font ships from **DejaVu Sans Mono** at the default metrics
(`px=10 baseline=6 xpad=0 gain=1.2`), which fill the 6×8 cell — a smaller `px`
leaves caps short and the text reads faint:

```bash
./build/fontgen /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
    ../../components/ui/src/font_data.cpp
```

| arg        | meaning                                                             |
|------------|--------------------------------------------------------------------|
| `px`       | nominal pixel height passed to stb (≈10 fills caps to the cell top) |
| `baseline` | cell row of the glyph baseline; descenders fall below it           |
| `xpad`     | extra left inset inside the 5px visible glyph box                  |
| `gain`     | coverage multiplier; mild boost so thin stems aren't washed out    |

Quick metric tuning without a rebuild: pass `preview:SOME TEXT` as the output
path to render the glyphs as terminal ASCII shading.

Cell geometry (6×8, ASCII 0x20..0x7E) is fixed in `fontgen.cpp` and must match
`components/ui/src/font.h`. After regenerating, rebuild the emulator to preview:
`cd emulator && cmake --build build && ./build/pixfrog_emu`.

`stb_truetype.h` is vendored here (public domain, v1.26).
