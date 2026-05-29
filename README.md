# pixfrog

> High-performance ArtNet → LED driver for ESP32-P4. 8 channels × 2 lines (DATA + CLOCK), supporting 1-wire protocols (WS2815, WS2812B, SK6812…) and clocked protocols (APA102, SK9822, LPD8806).

## Features

- ESP32-P4 dual-core RISC-V at 400 MHz, 32 MB octal PSRAM
- 10/100M Ethernet via external PHY (IP101GRI)
- 8 parallel LED channels driven by LCD_CAM 16-bit + GDMA
- ArtNet UDP receiver (up to 48 universes, with ArtPoll/ArtSync support)
- Local UI only: SSD1306 OLED + Adafruit seesaw rotary encoder
- All configuration is **local** and **persisted** in NVS — no network config surface

## Target hardware

The default board is the **Waveshare ESP32-P4 Module DEV-KIT**.
Pinout, schematic, datasheet, and PHY wiring are documented by Waveshare:

→ <https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT/Resources-And-Documents>

Any other ESP32-P4 board with octal PSRAM and an MII/RMII PHY works; just clone `boards/esp32_p4_devkit.h` and adjust the GPIO map.

## Build

ESP-IDF v5.3 or v5.4 with ESP32-P4 support (`idf.py set-target esp32p4`).

```bash
idf.py set-target esp32p4
idf.py menuconfig          # optional — sdkconfig.defaults is sane
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Host unit tests

Three pure-C++ test suites that run anywhere a C++17 compiler is installed (no IDF required):

```bash
# LED encoders + timings + encode throughput
cd components/led_protocols/test && cmake -B build && cmake --build build && ./build/test_led_protocols

# DMX manager logic (sizing, capacity, multi-universe decoder)
cd components/dmx_manager/test  && cmake -B build && cmake --build build && ./build/test_dmx_logic

# ArtNet parser (header, ArtDmx, net/subnet filter, ArtPollReply)
cd components/artnet/test       && cmake -B build && cmake --build build && ./build/test_artnet_parser
```

## CI

`.github/workflows/ci.yml` runs on every push:

- the three host suites above
- `idf.py build` for `esp32p4` in `espressif/idf:v5.3` and `v5.4` containers
- `clang-format --dry-run` against `.clang-format` on every tracked C/C++ file

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — task topology, frame lifecycle, memory budget
- [docs/HARDWARE.md](docs/HARDWARE.md) — pinout, PHY, level shifters, encoder + OLED wiring
- [docs/PROTOCOLS.md](docs/PROTOCOLS.md) — per-protocol timings, PCLK formula, DMA encoding
- [CLAUDE.md](CLAUDE.md) — conventions for working in this repo (humans and agents)

## Status

Firmware feature-complete against the spec. The IDF-side surface (LCD_CAM, SSD1306, seesaw, Ethernet) is written against documented IDF v5.3+ APIs but has not been validated on silicon — CI builds the binary for both IDF versions on every push.

## License

TBD.
