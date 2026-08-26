# pixfrog

> High-performance ArtNet / sACN → LED driver for ESP32-P4. 8 channels × 2 lines (DATA + CLOCK), supporting 1-wire protocols (WS2815, WS2812B, SK6812…), clocked protocols (APA102, SK9822, LPD8806) — or a DMX512 universe output per channel.

## Features

- ESP32-P4 dual-core RISC-V at 360 MHz, 32 MB octal PSRAM
- 10/100M Ethernet via external PHY (IP101GRI)
- 8 parallel LED channels on a 16-bit bus — **PARLIO TX loop DMA** (default) or legacy LCD_CAM backend
- **ArtNet 4** receiver: ArtDmx/ArtPoll/ArtSync, remote config via ArtAddress + ArtIpProg, scene triggers via ArtTrigger, ArtTimeCode sync (up to 48 universes)
- **sACN (E1.31)** receiver (opt-in): multicast joins per configured universe, per-universe priority, stream-terminate handling
- **2-source merge** (HTP/LTP) when ArtNet and sACN feed the same universe
- **FSEQ player**: `.fseq` sequences (xLights/FPP, zstd-compressed) from microSD with hot-plug, uploaded over the web UI, free-running or slaved to ArtTimeCode / **FPP MultiSync**
- **Standalone scenes**: 8 parametric slots (solid / chase / rainbow, per-channel mask), playable at boot, from the desk (ArtTrigger), or any UI
- **Signal-loss failsafe** per channel: hold / blackout / solid colour / scene after a configurable timeout
- **Per-channel gamma + white balance**, baked into encode-time LUTs (validated bit-exact on a logic analyzer)
- Local UI: **NV3007 428×142 colour bar TFT** (SPI, 2.79") with a live status dashboard (per-channel activity, link/services state) and natively rasterised anti-aliased fonts; Adafruit seesaw rotary encoder (4-wire I2C, time-polled); pixel-count live preview and strip-identify blink for commissioning. Two other panels are supported as build-time alternates — see [Display backend](#display-backend)
- **Web UI** (opt-in, port 80): full configuration SPA + REST API, live `/api/status` telemetry, **OTA firmware update** (A/B slots with boot-failure rollback), config **backup/restore** as JSON, crash **coredump download**, mDNS discovery while enabled, optional HTTP Basic auth on every mutation
- **UART control console**: every config field, telemetry, DMX injection, buffer readback (`tools/uartctl.sh`)
- All configuration **persisted** in NVS with forward migration; network surfaces beyond ArtNet are **strictly opt-in** (no socket while disabled)

## Target hardware

The default board is the **Waveshare ESP32-P4 Module DEV-KIT**.
Pinout, schematic, datasheet, and PHY wiring are documented by Waveshare:

→ <https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT/Resources-And-Documents>

Any other ESP32-P4 board with octal PSRAM and an MII/RMII PHY works; just clone `boards/esp32_p4_devkit.h` and adjust the GPIO map.

## Build & flash

ESP-IDF v5.5+ with ESP32-P4 support. `sdkconfig.defaults` already pins the
target, so no `set-target` is needed. Both `led_output` backends (PARLIO TX
default, legacy LCD_CAM RGB panel) need drivers that only ship from v5.5.

```bash
idf.py build                              # default: NV3007 bar panel
idf.py -p /dev/ttyACM0 flash              # write it to the board
idf.py -p /dev/ttyACM0 monitor            # boot log (Ctrl-] to leave)
idf.py menuconfig                         # optional — sdkconfig.defaults is sane
```

Without a local IDF, build and flash in the official container:

```bash
docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
    espressif/idf:v5.5 idf.py build
docker run --rm --device /dev/ttyACM0 -v "$PWD":/project -w /project \
    espressif/idf:v5.5 idf.py -p /dev/ttyACM0 flash
```

> `sdkconfig` is generated and git-ignored. It is only seeded from
> `sdkconfig.defaults` **when it does not exist**, so after changing the defaults
> or switching variant, `rm -f sdkconfig` first — otherwise the old selection
> silently survives.

### Display backend

The default is the **NV3007 428×142 bar panel** the 1U rack ships with — a plain
`idf.py build` targets it, and the boot log confirms with
`TFT: NV3007 428x142 ready (landscape)`.

Two alternates are supported for other builds of the hardware. Each needs its own
build directory and sdkconfig, or the overlay is silently ignored:

| Panel | Overlay | Build |
|---|---|---|
| **NV3007** 428×142 SPI bar (2.79") | *(default)* | `idf.py build` |
| ST7789V / ILI9341 320×240 SPI | `sdkconfig.ci.st7789` | `idf.py -B build.st7789 -D SDKCONFIG=build.st7789/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.st7789" build` |
| SSD1306 128×64 I²C OLED | `sdkconfig.ci.oled` | `idf.py -B build.oled -D SDKCONFIG=build.oled/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.oled" build` |

Flash a variant from its own directory, e.g. `idf.py -B build.oled -p /dev/ttyACM0 flash`.
`PIXFROG_NV3007_ROT180` flips the bar panel when the module is mounted upside down.

TFT SPI GPIOs live in `boards/esp32_p4_devkit.h` — CLK=0, MOSI=6, CS=20, DC=21,
RST=27 on the shield's J13 header, backlight on GPIO 45, SPI2 at 20 MHz. See
[docs/HARDWARE.md §5](docs/HARDWARE.md).

## Host unit tests

Seven pure-C++ test suites that run anywhere a C++17 compiler is installed (no IDF required):

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

# FSEQ parser (header, sparse ranges, zstd frames)
cd components/fseq_player/test  && cmake -B build && cmake --build build && ./build/test_fseq_parser

# FPP MultiSync parser
cd components/fpp_sync/test     && cmake -B build && cmake --build build && ./build/test_fpp_sync_parser
```

There is also an SDL2 **UI emulator** (`tools/emulator`) that compiles the real
menu FSM for the host and drives it headlessly (`tools/emulator/smoke.sh`), and
a replayable **hardware regression suite** (`tools/hw_validate`) that proves
every feature on the real board after a flash.

## CI

`.github/workflows/ci.yml` runs on pushes to `main` and on pull requests
targeting `main`:

- the seven host suites above
- the SDL2 emulator build + headless menu-FSM smoke test
- `idf.py build` for `esp32p4` × **three display variants** (nv3007 default, st7789, oled) in the `espressif/idf:v5.5` container
- `clang-format --dry-run` against `.clang-format` on every tracked C/C++ file

`./tools/ci-local.sh` replays all of it locally and must be green before any push.

## Releases & browser flashing

Firmware and the website ship on independent cadences:

- `.github/workflows/release.yml` runs on every `v*` tag (e.g. `git tag v0.1.0
  && git push origin v0.1.0`): it builds the firmware and publishes a GitHub
  Release with `pixfrog-merged.bin` (flash at `0x0`) plus the individual
  bootloader / partition-table / app parts.
- `.github/workflows/pages.yml` runs on every push to `main` that touches the
  site (`.github/pages/**`, `docs/**`): it deploys the
  [esp-web-tools](https://esphome.github.io/esp-web-tools/) flasher and docs to
  GitHub Pages so the board can be flashed from desktop Chrome/Edge over USB —
  no toolchain required.

The flasher's `manifest.json` pins the **latest** Release asset
(`releases/latest/download/pixfrog-merged.bin`), so the site always serves the
newest firmware without a redeploy — a copy or CSS edit goes live on a plain
push to `main`, with no firmware version bump.

One-time setup: enable **Settings → Pages → Source: GitHub Actions** (the
workflow also attempts to enable it automatically).

## Documentation

Rendered online (with the browser flasher) at **<https://lefix2.github.io/pixfrog/>**:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — task topology, frame lifecycle, memory budget
- [docs/HARDWARE.md](docs/HARDWARE.md) — pinout, PHY, level shifters, encoder + display wiring
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
ArtNet, sACN (unicast), OTA, auth, failsafe, scenes, backup/restore and FSEQ
playback (upload, seek, ArtTimeCode, FPP MultiSync) exercised end-to-end on
the board — captured as the replayable suite in `tools/hw_validate`.
Remaining field items live in [TODO.md](TODO.md).

## License

pixfrog is released under the [MIT License](LICENSE) — © 2026 Lefix2.
