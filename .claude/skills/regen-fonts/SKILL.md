---
name: regen-fonts
description: Regenerate the UI fonts (TFT font_data.cpp from TTF, OLED font_oled.cpp from gen.py) — never hand-edit them
---

TFT (anti-aliased 6×8 + 12×16 cells from a TTF; canonical small-cell metrics
`9 6 0 2`, large cell uses the built-in defaults):
```bash
cd tools/fontgen && cmake -B build && cmake --build build
./build/fontgen /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf ../../components/ui/src/font_data.cpp 9 6 0 2
```

OLED (crisp 1bpp 5×7, hand-drawn table):
```bash
python3 tools/oledfont/gen.py   # writes components/ui/src/font_oled.cpp
```

Cell geometry (6×8 base, 12×16 = exactly 2× for the TFT's even text scales) is
fixed in both `fontgen.cpp` and `font.h` and must stay in sync — `menu.cpp`
layout maths run in small-cell units. READMEs: tools/fontgen, tools/oledfont.
