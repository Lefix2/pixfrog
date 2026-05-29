# CLAUDE.md

Quick orientation for any agent (or human) working in this repo.

## What this is

Firmware for an 8-channel ArtNet → LED driver on ESP32-P4. Detailed design in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); hardware in [docs/HARDWARE.md](docs/HARDWARE.md); per-protocol timings in [docs/PROTOCOLS.md](docs/PROTOCOLS.md); target board ([Waveshare ESP32-P4 Module DEV-KIT](https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT/Resources-And-Documents)).

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
| `components/led_protocols`     | Pure C++ NRZ + SPI encoders (host-testable)             |
| `components/lcd_cam_output`    | LCD_CAM 16-bit driver, PSRAM double-buffer, calibration |
| `components/artnet`            | UDP receiver + parser + ArtPollReply                    |
| `components/dmx_manager`       | Universe pool, channel mapping, capacity check, sync    |
| `components/config_store`      | NVS-backed `GlobalConfig` + `ChannelConfig`             |
| `components/ui`                | SSD1306 driver, seesaw encoder, menu FSM                |
| `main/main.cpp`                | Boot orchestration + `render_task`                      |

## Tests

Three pure host suites (no IDF needed):

```bash
cd components/led_protocols/test && cmake -B build && cmake --build build && ./build/test_led_protocols
cd components/dmx_manager/test  && cmake -B build && cmake --build build && ./build/test_dmx_logic
cd components/artnet/test       && cmake -B build && cmake --build build && ./build/test_artnet_parser
```

When you change anything in `led_protocols`, `dmx_manager`, or `artnet`, the matching suite must still pass.

When you refactor IDF-bound code, the canonical proof is `idf.py build` — locally if IDF is installed, otherwise let CI verify (`.github/workflows/ci.yml` runs `idf.py build` against v5.3 and v5.4).

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

`sdkconfig.defaults` already pins the values that matter (octal PSRAM, 400 MHz CPU, no tickless idle, lwIP UDP tuning, partition table). Use `menuconfig` only when you mean to change them deliberately.

## Don'ts

- Don't add features that aren't on the user's TODO list.
- Don't add WiFi, mDNS, or any extra network surface — design rule, ArtNet UDP is the only one.
- Don't write to NVS from `render_task` or ISRs — only from `ui_task`.
- Don't put a frame-buffer-sized allocation in SRAM — that lives in PSRAM.
- Don't sleep in ISRs, don't `printf` in ISRs, don't grab mutexes in ISRs.
- Don't add a comment block to "summarize" what a function does when the name + types already say it.
