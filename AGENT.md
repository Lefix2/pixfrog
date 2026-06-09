# AGENT.md

Quick orientation for any agent (or human) working in this repo.

## What this is

Firmware for an 8-channel ArtNet → LED driver on ESP32-P4. Each channel drives an LED strip (WS/SK NRZ or APA/SK/LPD clocked SPI) or, alternatively, a single DMX512 universe output. Detailed design in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); hardware in [docs/HARDWARE.md](docs/HARDWARE.md); per-protocol timings (incl. DMX512 §7) in [docs/PROTOCOLS.md](docs/PROTOCOLS.md); target board ([Waveshare ESP32-P4 Module DEV-KIT](https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT/Resources-And-Documents)).

## Conventions

- **C++17**, no exceptions, no RTTI. Don't add `try`/`catch` or `dynamic_cast`.
- **No designated initializers** (`{.field = …}`) outside of `main.cpp` and IDF struct fills — they're a GCC extension under `-std=c++17` and break `-Wpedantic` host builds.
- **Comments**: don't restate what the code does. Only write a comment when WHY is non-obvious (hidden invariant, datasheet quirk, workaround). PR descriptions are not stored in code.
- **No allocation on the hot path**. Everything `render_task` / ISR touches is allocated at boot.
- **ISRs are `IRAM_ATTR`** and only `xSemaphoreGiveFromISR` / increment a counter.
- **Atomic pointer swaps** for cross-thread data, not mutexes. See `dmx_manager` for the pattern.

## Module map

| Path                           | Responsibility                                          |
|--------------------------------|---------------------------------------------------------|
| `boards/esp32_p4_devkit.h`     | Single source of truth for pinout / I2C addrs           |
| `components/led_protocols`     | Pure C++ NRZ + SPI + DMX512 encoders (host-testable)    |
| `components/lcd_cam_output`    | LCD_CAM 16-bit driver, PSRAM double-buffer, calibration |
| `components/artnet`            | UDP receiver + parser + ArtPollReply                    |
| `components/dmx_manager`       | Universe pool, channel mapping, capacity check, sync    |
| `components/config_store`      | NVS-backed `GlobalConfig` + `ChannelConfig`             |
| `components/ui`                | SSD1306 driver, seesaw encoder, menu FSM                |
| `main/main.cpp`                | Boot orchestration + `render_task`                      |
| `tools/fontgen`                | Host tool: TTF → `font_data.cpp` (anti-aliased font)    |
| `tools/splashgen`              | Host tool: `frog-anim.svg` → `splash_anim.cpp` (frog anim)|
| `tools/emulator`               | SDL2 host emulator of the TFT UI (no IDF)               |
| `docs/img/frog-anim.svg`       | Animated logo: source for web + baked splash            |

## Tests

Three pure host suites (no IDF needed):

```bash
cd components/led_protocols/test && cmake -B build && cmake --build build && ./build/test_led_protocols
cd components/dmx_manager/test  && cmake -B build && cmake --build build && ./build/test_dmx_logic
cd components/artnet/test       && cmake -B build && cmake --build build && ./build/test_artnet_parser
```

When you change anything in `led_protocols`, `dmx_manager`, or `artnet`, the matching suite must still pass.

When you refactor IDF-bound code, the canonical proof is `idf.py build` — locally if IDF is installed, otherwise let CI verify (`.github/workflows/ci.yml` runs `idf.py build` against v5.5). ESP32-P4 requires IDF v5.5+ (the P4 LCD_CAM RGB panel driver is unavailable before then).

## UI emulator

`tools/emulator/` runs the real TFT UI (`menu.cpp`, `canvas_tft.cpp`, `splash.cpp`, `font_data.cpp`) on a host via SDL2 — preview the interface without flashing, and drive it from an AI agent. It reimplements the one hardware seam (`tft_draw_bitmap`) over an SDL framebuffer and stubs the neighbour components in RAM. See `tools/emulator/README.md`.

```bash
sudo apt install libsdl2-dev
cd tools/emulator && cmake -B build && cmake --build build
./build/pixfrog_emu              # interactive window + keyboard
./build/pixfrog_emu --headless   # agent/CI: stdin protocol (left|right|click|shot|splash|state|quit)
```

`splash <ms> [path]` renders the boot splash at time `ms` and screenshots it
(the headless main loop otherwise starts straight at HOME and the menu repaints
every frame, so the splash can't be captured the normal way).

The only shared-code hook is `menu_debug_state()` in `menu.cpp`, guarded by `#ifdef PIXFROG_EMULATOR` — the firmware build never defines it. When you add a device call to `menu.cpp`, extend the matching `tools/emulator/src/*_host.cpp` stub.

## Font

The UI font is **generated**, not hand-edited. `components/ui/src/font_data.cpp`
holds an 8-bit anti-aliased coverage cell (6×8) per ASCII glyph; the TFT blends
fg→bg by coverage (`canvas_tft.cpp`), the OLED thresholds it to 1bpp
(`oled_ssd1306.cpp`). Regenerate from a TTF instead of touching the table:

```bash
cd tools/fontgen && cmake -B build && cmake --build build
./build/fontgen /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
    ../../components/ui/src/font_data.cpp        # px / baseline / xpad / gain are tunable
```

Cell geometry (6×8) is fixed in both `fontgen.cpp` and `font.h` and must stay in
sync — `menu.cpp` layout maths depends on `kFontCellWidth`/`kFontHeight`. The
vendored `stb_truetype.h` and the generated `font_data.cpp` are listed in
`.clang-format-ignore`. See `tools/fontgen/README.md`.

## Splash

The TFT boot splash replays the animated Collecti'Frog logo
`docs/img/frog-anim.svg` (frog surfaces from the water, bands settle, eyes
blink). That SVG is the **single source of truth**: the web page embeds it
(`.github/pages/about.html` via `<img>`, deployed as `site/img/`), and the splash
is **generated** from it — `tools/splashgen` bakes the SVG + its animation
timeline into 1bpp masks in `components/ui/src/splash_anim.cpp`, which `splash.cpp`
blits frame *t* via `canvas_draw_mask`. Regenerate from the SVG; don't hand-edit
`splash_anim.cpp`. See `tools/splashgen/README.md`. (`canvas_draw_text` caps a
text block at 24px, so the wordmark is scale 3, not 4.)

## Formatting

`clang-format` runs in CI. Format before committing:

```bash
git ls-files '*.h' '*.cpp' | xargs clang-format -i
```

Style is defined by `.clang-format` at the repo root. Don't bypass it.

## Build (IDF side)

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig.defaults` already pins the values that matter (octal PSRAM, 400 MHz CPU, no tickless idle, lwIP UDP tuning, partition table). Use `menuconfig` only when you mean to change them deliberately. The target is pinned (`CONFIG_IDF_TARGET="esp32p4"`), so a bare `idf.py build` picks it up without `set-target`.

### Build & flash without a local IDF (Docker)

If ESP-IDF isn't installed on the host, build and flash inside the official image
(`espressif/idf:v5.5` — v5.5+ is required for the P4 LCD_CAM driver):

```bash
# Build as your own uid so build/ isn't left root-owned:
docker run --rm -v "$PWD":/project -w /project \
    -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 idf.py build

# Flash/monitor must run as root with the port mapped in (--device), because the
# in-container uid isn't in the host 'dialout' group either:
docker run --rm --device /dev/ttyACM0 -v "$PWD":/project -w /project \
    espressif/idf:v5.5 idf.py -p /dev/ttyACM0 flash
```

After a root flash, `build/` may contain root-owned files; `docker run --rm -v "$PWD":/project espressif/idf:v5.5 chown -R "$(id -u):$(id -g)" /project/build` cleans it.

### Serial port & USB notes

- The CH343 USB-serial bridge (`1a86:55d3`) on the dev board enumerates as
  **`/dev/ttyACM0`**, not `/dev/ttyUSB0`. Pass `-p /dev/ttyACM0`.
- The host user is typically not in `dialout`; either `sudo usermod -aG dialout
  $USER` (re-login) or flash via the root Docker path above.
- `idf.py monitor` inside non-interactive Docker doesn't reset the chip, so it
  misses the boot log. To capture a clean boot, pulse RTS (EN) then read — e.g. a
  short pyserial snippet (`s.rts=True; sleep; s.rts=False; read`).
- **WSL2**: unplugging/replugging the board drops the usbip attachment. After a
  physical reconnect the device won't reappear in Linux until it's re-bound from
  Windows: `usbipd list` then `usbipd attach --wsl --busid <BUSID>`.

## Don'ts

- Don't add features that aren't on the user's TODO list.
- Don't add WiFi, mDNS, or any extra network surface — design rule, ArtNet UDP is the only one.
- Don't write to NVS from `render_task` or ISRs — only from `ui_task`.
- Don't put a frame-buffer-sized allocation in SRAM — that lives in PSRAM.
- Don't sleep in ISRs, don't `printf` in ISRs, don't grab mutexes in ISRs.
- Don't add a comment block to "summarize" what a function does when the name + types already say it.
