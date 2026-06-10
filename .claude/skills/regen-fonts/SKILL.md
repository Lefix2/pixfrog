---
name: regen-fonts
description: Regenerate the UI fonts (TFT font_data.cpp from TTF, OLED font_oled.cpp from gen.py) — never hand-edit them
---

TFT (anti-aliased 6×8 from a TTF):
```bash
cd tools/fontgen && cmake -B build && cmake --build build
./build/fontgen /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf ../../components/ui/src/font_data.cpp
```

OLED (crisp 1bpp 5×7, hand-drawn table):
```bash
python3 tools/oledfont/gen.py   # writes components/ui/src/font_oled.cpp
```

Cell geometry (6×8) is fixed in both `fontgen.cpp` and `font.h` and must stay in sync — `menu.cpp` layout depends on it. READMEs: tools/fontgen, tools/oledfont.
