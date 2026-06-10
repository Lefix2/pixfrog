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
| `tools/fontgen`                | Host tool: TTF → `font_data.cpp` (anti-aliased TFT font) |
| `tools/oledfont`               | Host tool: hand-drawn 5×7 → `font_oled.cpp` (crisp 1bpp OLED font) |
| `tools/splashgen`              | Host tool: `frog-anim.svg` → `splash_anim.cpp` (TFT frog anim)|
| `tools/oledsplash`             | Host tool: TFT splash frame → `splash_oled.cpp` (static OLED frog logo) |
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

The UI font is **generated**, not hand-edited. There are two:

- **TFT** uses `components/ui/src/font_data.cpp` — an 8-bit anti-aliased coverage
  cell (6×8) per ASCII glyph; `canvas_tft.cpp` blends fg→bg by coverage. Built by
  `tools/fontgen` from a TTF.
- **OLED** uses `components/ui/src/font_oled.cpp` — a crisp 1bpp 5×7 column-major
  table drawn for the pixel grid; `oled_ssd1306.cpp` copies a glyph's column
  bytes straight into the page framebuffer (no thresholding). Built by
  `tools/oledfont/gen.py` (see `tools/oledfont/README.md`).

Regenerate the TFT font from a TTF instead of touching the table:

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

The **OLED** can't fit the full animation, so its splash is **static**: a small
1bpp frog logo (`splash_oled.cpp`, baked by `tools/oledsplash` from one TFT frame)
above a scaled "pixfrog" wordmark, held ~1.8 s and click-skippable
(`splash.cpp`, `#else` branch). See `tools/oledsplash/README.md`.

## Formatting

`clang-format` runs in CI. Format before committing:

```bash
git ls-files '*.h' '*.cpp' | xargs clang-format -i
```

Style is defined by `.clang-format` at the repo root. Don't bypass it.

## Build & flash (Docker)

Build and flash inside the official ESP-IDF image (`espressif/idf:v5.5` — v5.5+
is required for the P4 LCD_CAM driver). No local IDF install needed.

```bash
# Build — run as your own uid so build/ isn't left root-owned:
docker run --rm -v "$PWD":/project -w /project \
    -u "$(id -u):$(id -g)" -e HOME=/tmp espressif/idf:v5.5 idf.py build

# Flash — run as root with the port mapped in (--device), because the
# in-container uid isn't in the host 'dialout' group. Replace $PORT with the
# board's serial device (find it with `ls /dev/ttyACM* /dev/ttyUSB*`):
docker run --rm --device "$PORT" -v "$PWD":/project -w /project \
    espressif/idf:v5.5 idf.py -p "$PORT" flash
```

The target is pinned (`CONFIG_IDF_TARGET="esp32p4"` in `sdkconfig.defaults`), so
a bare `idf.py build` needs no `set-target`. `sdkconfig.defaults` also pins octal
PSRAM, 400 MHz CPU, no tickless idle, lwIP UDP tuning and the partition table —
use `menuconfig` only to change them deliberately.

After a root flash `build/` may hold root-owned files; reclaim with
`docker run --rm -v "$PWD":/project espressif/idf:v5.5 chown -R "$(id -u):$(id -g)" /project/build`.

Notes:
- `idf.py monitor` inside non-interactive Docker doesn't reset the chip, so it
  misses the boot log. To capture a clean boot, pulse RTS (EN) then read — a
  short pyserial snippet (`s.rts=True; sleep; s.rts=False; read`) does it.
- **WSL2**: unplugging/replugging the board drops the usbip attachment; the port
  won't reappear in Linux until it's re-bound from Windows
  (`usbipd list` then `usbipd attach --wsl --busid <BUSID>`).

## Don'ts

- Don't add features that aren't on the user's TODO list.
- Don't add WiFi, mDNS, or any extra network surface — design rule, ArtNet UDP is the only one.
- Don't write to NVS from `render_task` or ISRs — only from `ui_task`.
- Don't put a frame-buffer-sized allocation in SRAM — that lives in PSRAM.
- Don't sleep in ISRs, don't `printf` in ISRs, don't grab mutexes in ISRs.
- Don't add a comment block to "summarize" what a function does when the name + types already say it.
