---
name: emulator
description: Build and drive the SDL2 host emulator of the TFT UI (headless stdin protocol for agents)
---

```bash
cd tools/emulator && cmake -B build && cmake --build build
./build/pixfrog_emu --headless   # stdin: left|right|click|shot|splash|state|quit
```

Drive it (one command per line on stdin; `shot [path]` writes a screenshot, `state` dumps the menu FSM):
```bash
printf 'right\nclick\nshot /tmp/ui.png\nstate\nquit\n' | ./build/pixfrog_emu --headless
```

`splash <ms> [path]` screenshots the boot splash at time *ms*. Needs `libsdl2-dev`. When adding a device call to `menu.cpp`, extend the matching `tools/emulator/src/*_host.cpp` stub. See tools/emulator/README.md.
