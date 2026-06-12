# pixfrog UI emulator

Runs the device's **real** UI code (`menu.cpp`, `canvas_tft.cpp`, `splash.cpp`,
`splash_anim.cpp`, `font_data.cpp`) on a PC against an SDL2 framebuffer instead
of the ST7789 TFT.
Lets you test the interface before flashing, and gives an AI agent a programmatic
way to drive and observe the UI.

## How it works

The UI only touches hardware through one function — `tft_draw_bitmap()`. The
emulator reimplements that (`src/tft_sdl.cpp`) over an in-RAM 320×240 RGB565
framebuffer, replaces the I2C rotary encoder with a host event queue
(`src/encoder_host.cpp`), and stubs the neighbour components the menu reads
(`config_store`, `dmx_manager`, `led_output`) with in-RAM versions that
mirror the device defaults. Everything else is the device's own source,
compiled with `-DCONFIG_PIXFROG_DISPLAY_TFT`.

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
| `set link up/down` | set link state (HOME)                              |
| `set fps <n>`      | inject a fake FPS counter (HOME)                   |
| `set active <ch>`  | mark channel `ch` (0–7) active (HOME dot)          |
| `quit`             | exit                                               |

### Example

```sh
printf 'click\nright\nright\nclick\nshot /tmp/chan.bmp\nstate\nquit\n' \
  | ./build/pixfrog_emu --headless
# → ok shot /tmp/chan.bmp
#   {"screen":"ChannelMenu","cursor":0,"channel":0}
```
