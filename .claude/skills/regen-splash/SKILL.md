---
name: regen-splash
description: Regenerate the boot splash (TFT anim from frog-anim.svg, static OLED logo) — never hand-edit splash_*.cpp
---

`docs/img/frog-anim.svg` is the single source of truth (web page + splash).

TFT animation → `components/ui/src/splash_anim.cpp`:
```bash
cd tools/splashgen && cmake -B build && cmake --build build
./build/splashgen ../../docs/img/frog-anim.svg \
    ../../components/ui/src/splash_anim.cpp        # [outW] [fps] [durMs] tunable
```

Static OLED logo → `components/ui/src/splash_oled.cpp`: multi-step (stub i2c header, bakes one TFT frame) — follow tools/oledsplash/README.md exactly.

Preview via the /emulator skill (`splash <ms>` stdin command).
