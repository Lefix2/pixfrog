# pixfrog UI emulator

Runs the device's **real** UI code (`menu.cpp`, `canvas_tft.cpp`, `splash.cpp`,
`splash_anim.cpp`, `font_data.cpp`) on a PC against an SDL2 framebuffer instead
of the ST7789 TFT.
Lets you test the interface before flashing, and gives an AI agent a programmatic
way to drive and observe the UI.

## How it works

The UI only touches hardware through one function — `tft_draw_bitmap()`. The
emulator reimplements that (`src/tft_sdl.cpp`) over an in-RAM RGB565
framebuffer, replaces the I2C rotary encoder with a host event queue
(`src/encoder_host.cpp`), and stubs the neighbour components the menu reads
(`config_store`, `dmx_manager`, `led_output`) with in-RAM versions that
mirror the device defaults. Everything else is the device's own source,
compiled with the same display macros as the firmware.

Two panel layouts exist, mirroring the firmware's TFT panel choice:
**NV3007 428×142** (the default build) and **ST7789 320×240**
(`cmake -B build.st7789 -DPIXFROG_EMU_PANEL=st7789`). CI builds and
smoke-tests both.

The only change to shared device code is `det::menu_debug_state()` in `menu.cpp`,
guarded by `#ifdef PIXFROG_EMULATOR` (invisible to the firmware build).

## Build & run

```sh
sudo apt install libsdl2-dev
cd tools/emulator && cmake -B build && cmake --build build

./build/pixfrog_emu               # interactive window (960×720, 3× zoom)
./build/pixfrog_emu --headless    # no window, piloted via stdin (agent/CI)
```

### Keyboard (interactive)

| Key                | Encoder event                |
|--------------------|------------------------------|
| ↑ / ← / wheel-up   | RotateLeft  (cursor up)      |
| ↓ / → / wheel-down | RotateRight (cursor down)    |
| Enter / Space      | Click                        |

## Agent / stdin protocol

One command per line on stdin; replies on stdout. Headless skips the splash and
starts at HOME for deterministic runs.

| Command            | Effect                                             |
|--------------------|----------------------------------------------------|
| `left` / `right`   | rotate the encoder                                 |
| `click`            | press the encoder                                  |
| `shot <path>`      | save the 320×240 framebuffer as BMP → `ok shot …`  |
| `splash <ms> [p]`  | render the boot splash at t=`ms` and shot it       |
| `state`            | print `{"screen":..,"cursor":..,"channel":..}`     |
| `set ip a.b.c.d`   | set the displayed IP (HOME)                        |
| `set net <state>`  | set the network icon: `disconnected`/`acquiring`/`connected`/`error` (HOME) |
| `set fps <n>`      | inject a fake FPS counter (HOME)                   |
| `set pkts <n>`     | inject a fake ArtNet RX packet counter (HOME)      |
| `set active <ch>`  | mark channel `ch` (0–7) active (HOME dot)          |
| `set chan <i> <proto> <uni> <pix>` | seed channel `i` (protocol enum value) |
| `set gaps <i> [<pos0>:<len> …]` | replace channel `i`'s dead-pixel gaps |
| `set patch <i> <pixel_map> <fixture_ctl> [<uni> [<dmx>]]` | what drives channel `i` from the network (0/1 each), and its fixtures' address |
| `set fixtures <i> [<pos0>:<len>[:r][:p<n>] …]` | replace channel `i`'s fixtures (`r` = mounted the other way round, `p<n>` = DMX profile n) |
| `set scenes <n>` | cut the scene list down to n |
| `dump chan <i>` / `dump scene <i>` | one JSON line of what the menu stored: a channel's pixels, layout and fixtures; a scene's name, group and parts |
| `set sd <n>`       | fake a microSD with n `.fseq` files (0 = none)     |
| `quit`             | exit                                               |

### Menu crawler and golden screenshots

`crawl.py` seeds rich state, snaps a few static screens and compares them pixel
for pixel with `golden/<panel>/*.png`, then walks every menu node, activates
every row (commits value edits, cancels the others) and fails if a node or an
editor kind was never reached. CI runs it on both panels; `tools/coverage.py`
runs it too (it brings `menu.cpp` above 80 %).

```sh
python3 tools/emulator/crawl.py build/pixfrog_emu
python3 tools/emulator/crawl.py build.st7789/pixfrog_emu --panel st7789
python3 tools/emulator/crawl.py build/pixfrog_emu --update-golden  # intended UI change
```

A mismatch leaves the new render in the temp dir (`golden-<panel>-<name>.png`).
Needs Pillow (`pip install pillow`).

### Example

```sh
printf 'click\nright\nright\nclick\nshot /tmp/chan.bmp\nstate\nquit\n' \
  | ./build/pixfrog_emu --headless
# → ok shot /tmp/chan.bmp
#   {"screen":"ChannelMenu","cursor":0,"channel":0}
```
