# pixfrog

> High-performance ArtNet / sACN → LED driver for ESP32-P4. 8 channels × 2 lines (DATA + CLOCK), supporting 1-wire protocols (WS2815, WS2812B, SK6812…) and clocked protocols (APA102, SK9822, LPD8806).

## Features

- ESP32-P4 dual-core RISC-V at 400 MHz, 32 MB octal PSRAM
- 10/100M Ethernet via external PHY (IP101GRI)
- 8 parallel LED channels on a 16-bit bus — **PARLIO TX loop DMA** (default) or legacy LCD_CAM backend
- **ArtNet 4** receiver: ArtDmx/ArtPoll/ArtSync, remote config via ArtAddress + ArtIpProg, scene triggers via ArtTrigger (up to 48 universes)
- **sACN (E1.31)** receiver (opt-in): multicast joins per configured universe, per-universe priority, stream-terminate handling
- **Standalone scenes**: 8 parametric slots (solid / chase / rainbow, per-channel mask), playable at boot, from the desk (ArtTrigger), or any UI
- **Signal-loss failsafe** per channel: hold / blackout / solid colour / scene after a configurable timeout
- **Per-channel gamma + white balance**, baked into encode-time LUTs (validated bit-exact on a logic analyzer)
- Local UI: **compile-time selectable** — SSD1306 OLED 128×64 (default) **or** ST7789V TFT 320×240 colour (SPI), shared canvas API and menu FSM; Adafruit seesaw rotary encoder (4-wire I2C, time-polled); pixel-count live preview and strip-identify blink for commissioning
- **Web UI** (opt-in, port 80): full configuration SPA + REST API, **OTA firmware update** (A/B slots with boot-failure rollback), config **backup/restore** as JSON, optional HTTP Basic auth on every mutation
- **UART control console**: every config field, telemetry, DMX injection, buffer readback (`tools/uartctl.sh`)
- All configuration **persisted** in NVS with forward migration; network surfaces beyond ArtNet are **strictly opt-in** (no socket while disabled)

## Target hardware

The default board is the **Waveshare ESP32-P4 Module DEV-KIT**.
Pinout, schematic, datasheet, and PHY wiring are documented by Waveshare:

→ <https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT/Resources-And-Documents>

Any other ESP32-P4 board with octal PSRAM and an MII/RMII PHY works; just clone `boards/esp32_p4_devkit.h` and adjust the GPIO map.

## Build

ESP-IDF v5.5+ with ESP32-P4 support (`idf.py set-target esp32p4`). The P4
LCD_CAM RGB panel driver that `led_output` relies on is only available
from v5.5.

```bash
idf.py set-target esp32p4
idf.py menuconfig          # optional — sdkconfig.defaults is sane
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Display backend

Select at `idf.py menuconfig` → **Display backend** (or export `SDKCONFIG_DEFAULTS`):

| Kconfig symbol                | Display                      | Notes                          |
|-------------------------------|------------------------------|--------------------------------|
| `CONFIG_PIXFROG_DISPLAY_OLED` | SSD1306 128×64 I2C *(default)* | monochrome, diff-based flush  |
| `CONFIG_PIXFROG_DISPLAY_TFT`  | ST7789V 320×240 SPI landscape | 16-bit colour, animated splash |

TFT SPI GPIOs are configured in `boards/esp32_p4_devkit.h` (CLK=13, MOSI=11, CS=12, DC=10, RST=9, 40 MHz).

## Host unit tests

Five pure-C++ test suites that run anywhere a C++17 compiler is installed (no IDF required):

```bash
# LED encoders + timings + encode throughput
cd components/led_protocols/test && cmake -B build && cmake --build build && ./build/test_led_protocols

# DMX manager logic (sizing, capacity, multi-universe decoder)
cd components/dmx_manager/test  && cmake -B build && cmake --build build && ./build/test_dmx_logic

# ArtNet parser (header, ArtDmx/Nzs/Address/IpProg, filter, replies)
cd components/artnet/test       && cmake -B build && cmake --build build && ./build/test_artnet_parser

# sACN parser (root/framing/DMP, sync, per-universe priority gate)
cd components/sacn/test         && cmake -B build && cmake --build build && ./build/test_sacn_parser

# config store (struct layout, NVS forward migration)
cd components/config_store/test && cmake -B build && cmake --build build && ./build/test_config_store
```

There is also an SDL2 **UI emulator** (`tools/emulator`) that compiles the real
menu FSM for the host and drives it headlessly (`tools/emulator/smoke.sh`).

## CI

`.github/workflows/ci.yml` runs on pushes to `main` and on pull requests
targeting `main`:

- the five host suites above
- the SDL2 emulator build + headless menu-FSM smoke test
- `idf.py build` for `esp32p4` × **two display backends** (oled + tft) in the `espressif/idf:v5.5` container
- `clang-format --dry-run` against `.clang-format` on every tracked C/C++ file

`./tools/ci-local.sh` replays all of it locally and must be green before any push.

## Releases & browser flashing

`.github/workflows/release.yml` runs on every `v*` tag (e.g. `git tag v0.1.0
&& git push origin v0.1.0`). It builds the firmware, then:

- publishes a GitHub Release with `pixfrog-merged.bin` (flash at `0x0`) and the
  individual bootloader / partition-table / app parts;
- deploys a [esp-web-tools](https://esphome.github.io/esp-web-tools/) flasher to
  GitHub Pages so the board can be flashed from desktop Chrome/Edge over USB —
  no toolchain required.

One-time setup: enable **Settings → Pages → Source: GitHub Actions** (the
workflow also attempts to enable it automatically).

## Documentation

Rendered online (with the browser flasher) at **<https://lefix2.github.io/pixfrog/>**:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — task topology, frame lifecycle, memory budget
- [docs/HARDWARE.md](docs/HARDWARE.md) — pinout, PHY, level shifters, encoder + OLED wiring
- [docs/PROTOCOLS.md](docs/PROTOCOLS.md) — per-protocol timings, PCLK formula, DMA encoding
- [AGENT.md](AGENT.md) — conventions, module map, hard rules (humans and agents)
- [TODO.md](TODO.md) — the living roadmap; features land only from this list

## Hardware companion

`hardware/pixfrog_shield/` holds the KiCad project + JLCPCB production files of
the **pixfrog shield**: 2× 74HCT245 re-drive the 16 bus lines at 5 V,
DIP-selectable series termination, one TVS clamp per output, 8× JST-XH.
See [its README](hardware/pixfrog_shield/README.md).

## Status

Validated on silicon (Waveshare ESP32-P4 Module DEV-KIT): WS2815 NRZ timing,
gamma LUTs and frame content verified bit-exact on a Saleae logic analyzer;
ArtNet, sACN (unicast), OTA, auth, failsafe, scenes and backup/restore
exercised end-to-end on the board. Remaining field items live in
[TODO.md](TODO.md).

## License

pixfrog is released under the [MIT License](LICENSE) — © 2026 Lefix2.
