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
| `components/control_console`   | UART0 command server: full config get/set, telemetry, DMX injection |
| `main/main.cpp`                | Boot orchestration + `render_task`                      |
| `tools/fontgen`                | Host tool: TTF → `font_data.cpp` (anti-aliased TFT font) |
| `tools/oledfont`               | Host tool: hand-drawn 5×7 → `font_oled.cpp` (crisp 1bpp OLED font) |
| `tools/splashgen`              | Host tool: `frog-anim.svg` → `splash_anim.cpp` (TFT frog anim)|
| `tools/oledsplash`             | Host tool: TFT splash frame → `splash_oled.cpp` (static OLED frog logo) |
| `tools/emulator`               | SDL2 host emulator of the TFT UI (no IDF)               |
| `docs/img/frog-anim.svg`       | Animated logo: source for web + baked splash            |

## CI must pass locally before pushing

**Hard rule: replay every CI job locally and get it green before any push.**
`tools/ci-local.sh` runs the exact jobs of `.github/workflows/ci.yml` — the
clang-format check, the three host test suites, and both IDF builds (oled +
tft overlay):

```bash
./tools/ci-local.sh
```

Inside the devcontainer the IDF builds run natively; outside, the script falls
back to the same `espressif/idf:v5.5` docker image CI uses. Don't push and
"let CI find out".

## Devcontainer (CI parity)

`.devcontainer/` builds on the CI image (`espressif/idf:v5.5`) plus the tools
the other CI jobs use: clang-format **18** (pinned, same major as the
`format-check` job), build-essential/cmake for the host test suites, and
libsdl2-dev for the UI emulator. Every shell sources the IDF env, so
`idf.py build` works directly (no docker wrapper). The container runs
privileged with `/dev` mapped in, so `idf.py -p /dev/ttyACM0 flash` works
from inside it (it runs as root, which satisfies the flash-needs-root rule
below).

## Control console (UART)

`components/control_console` exposes a line-oriented command server on the
UART0 console (`/dev/ttyACM0` @ 115200, via the board's USB-UART bridge —
note: the bridge enumerates as CDC-ACM, but it is *not* the P4's native
USB-Serial-JTAG; UART0 is the only console wired to USB) — the full device is
scriptable for bench tests: every `GlobalConfig`/`ChannelConfig` field,
telemetry, calibration patterns, DMX injection without ArtNet, and buffer
readback to verify the universe→pixel pipeline end to end.

Protocol: each command answers `key=value` lines then a final `OK`, or one
`ERR <reason>` line. Boot logs share the port — send `loglevel none` first
when parsing strictly. Commands (see `help` on the device):

| Command | Purpose |
|---|---|
| `version` / `status` / `stats` / `chstat` | Versions; link/IP/MAC/FPS/heap; telemetry counters; per-channel active+capacity |
| `global [<key> <value>]` | Get/set GlobalConfig: `dhcp ip mask gw net subnet short_name long_name reply_unicast refresh_hz home_timeout_s` |
| `ch <n> [<key> <value>]` | Get/set ChannelConfig: `protocol order universe dmx_start pixels brightness grouping invert clock_hz` |
| `dmxw <uni> <slot> <hex>` | Inject DMX bytes into a mapped universe (1-based slot) |
| `dmxr <uni> [start len]` / `pixr <ch> [start len]` | Hex readback of universe / decoded pixel buffers |
| `cal [-1\|0\|1\|2]` | Get/set persistent calibration pattern |
| `loglevel <none..verbose>` / `factory-reset` / `reboot` | Logs and lifecycle |

Sets persist to NVS and take effect next frame (`mark_*_dirty`); network keys
(`dhcp ip mask gw`) need a `reboot`. **Opening the port can auto-reset the
board** (bridge DTR/RTS circuit), so sync on the prompt before scripting:

```python
import serial, time
s = serial.Serial("/dev/ttyACM0", 115200, timeout=1)
buf = ""
while "pixfrog>" not in buf:          # poke until the REPL answers (boot ~4 s)
    s.write(b"\r\n"); time.sleep(0.5)
    buf += s.read(s.in_waiting or 1).decode(errors="replace")
for c in (b"loglevel none", b"ch 0 protocol WS2812B", b"dmxw 1 1 ff0000", b"status"):
    s.write(c + b"\r\n"); time.sleep(0.3)
print(s.read(4096).decode())          # responses end with OK / ERR <reason>
```

Console writes share `ui_task`'s NVS path and are not locked against it —
don't turn the encoder while a script is configuring the device.

## Tests

Three pure host suites (no IDF needed):

```bash
cd components/led_protocols/test && cmake -B build && cmake --build build && ./build/test_led_protocols
cd components/dmx_manager/test  && cmake -B build && cmake --build build && ./build/test_dmx_logic
cd components/artnet/test       && cmake -B build && cmake --build build && ./build/test_artnet_parser
```

When you change anything in `led_protocols`, `dmx_manager`, or `artnet`, the matching suite must still pass.

When you refactor IDF-bound code, the canonical proof is `idf.py build` — natively in the devcontainer, or through the `espressif/idf:v5.5` docker image (`tools/ci-local.sh` does both display variants). Never rely on CI to find out. ESP32-P4 requires IDF v5.5+ (the P4 LCD_CAM RGB panel driver is unavailable before then).

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
