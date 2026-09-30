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

`splash <ms> [path]` screenshots the boot splash at time *ms* (the headless loop otherwise starts at HOME and repaints every frame). Needs `libsdl2-dev`.

Menu crawler + golden screenshots (CI; fails if a node is unreachable or a screen changed):
```bash
python3 tools/emulator/crawl.py build/pixfrog_emu [--panel st7789] [--update-golden]
```
A new menu node needs its name in `kNodeNames` (a `static_assert` checks the count) and in `REQUIRED` of `crawl.py`. `set chan`/`set gaps` seed state (README).

The only shared-code hook is `menu_debug_state()` in `menu.cpp`, guarded by `#ifdef PIXFROG_EMULATOR` — the firmware build never defines it. When adding a device call to `menu.cpp`, extend the matching `tools/emulator/src/*_host.cpp` stub. See tools/emulator/README.md.
