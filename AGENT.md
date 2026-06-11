# AGENT.md

Quick orientation for any agent (or human) working in this repo.

**Operational how-tos live in `.claude/skills/<name>/SKILL.md`** (plain
markdown, auto-loaded by Claude Code at the repo root, readable by anyone).
This file keeps the rules, invariants and design facts.

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
| `components/lcd_cam_output`    | 16-bit LED bus output: PARLIO TX loop (default) or LCD_CAM RGB backend (Kconfig choice), PSRAM double-buffer, calibration |
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
| `tools/uartctl.sh`             | One-shot client for the control console                 |
| `docs/img/frog-anim.svg`       | Animated logo: source for web + baked splash            |

## Workflows → skills

| Skill | Operation |
|---|---|
| [build](.claude/skills/build/SKILL.md) | IDF build (docker or devcontainer), oled + tft variants, sdkconfig traps |
| [flash](.claude/skills/flash/SKILL.md) | Flash over `/dev/ttyACM0`, boot-log capture, WSL2 replug |
| [host-tests](.claude/skills/host-tests/SKILL.md) | The three pure-host unit suites |
| [ci-local](.claude/skills/ci-local/SKILL.md) | Replay all CI jobs locally |
| [format](.claude/skills/format/SKILL.md) | clang-format (style: `.clang-format`, don't bypass) |
| [emulator](.claude/skills/emulator/SKILL.md) | SDL2 UI emulator, headless stdin protocol |
| [regen-fonts](.claude/skills/regen-fonts/SKILL.md) | Regenerate `font_data.cpp` / `font_oled.cpp` |
| [regen-splash](.claude/skills/regen-splash/SKILL.md) | Regenerate `splash_anim.cpp` / `splash_oled.cpp` |
| [uartctl](.claude/skills/uartctl/SKILL.md) | Drive the device over UART (config, telemetry, DMX injection) |

## Web configuration UI

`components/web_config`: optional HTTP server (port 80) with an embedded SPA + REST API.
Controlled by `GlobalConfig::web_enabled` (NVS-backed, default **off**). No TCP socket is
opened while the flag is off — opt-in only. Toggle from the Network submenu, via
`global web_enabled 0|1` in the UART console, or via POST `/api/global`.

REST endpoints: `GET /` (SPA), `GET /api/config`, `POST /api/global`, `POST /api/channel/{0..7}`,
`POST /api/reboot`, `POST /api/factory-reset`. All JSON.

This is the only TCP surface beyond ArtNet UDP — the strict "no extra network surface" rule
applies to *always-on* listeners; this one is opt-in and user-controlled.

## Hard rules

- **Never commit to `main`** — it is protected on GitHub (PR-only, CI checks
  required, no force-push). Every change, however small (fix, docs, tooling),
  goes on a feature branch (`feat/…`, `fix/…`, `docs/…`, `chore/…`) and lands
  through a pull request once CI is green.
- **CI must pass locally before any push**: `./tools/ci-local.sh` replays every
  `ci.yml` job (format check, three host suites, oled + tft + parlio IDF
  builds). Never push and "let CI find out".
- A change in `led_protocols`, `dmx_manager`, or `artnet` requires the matching
  host suite green.
- The canonical proof for IDF-bound refactors is `idf.py build` — natively in
  the devcontainer (`.devcontainer/`, built on the CI image `espressif/idf:v5.5`)
  or through that same docker image. ESP32-P4 requires IDF **v5.5+** (P4
  LCD_CAM RGB panel driver).

## Control console (UART)

`components/control_console`: line-oriented command server on the UART0
console — every config field get/set, telemetry, calibration, DMX injection
without ArtNet, universe/pixel buffer readback. Protocol: `key=value` lines
then `OK`, or one `ERR <reason>` line. Use `tools/uartctl.sh` (see the
[uartctl](.claude/skills/uartctl/SKILL.md) skill) rather than raw pyserial.

Hardware facts: `/dev/ttyACM0` is a **USB-UART bridge into UART0** (CDC-ACM,
but *not* the P4's native USB-Serial-JTAG), and **opening the port can
auto-reset the board** — sync on the `pixfrog>` prompt first (uartctl does).
Console config writes share `ui_task`'s NVS path and are not locked against
it — don't turn the encoder while a script is configuring the device.

## Generated sources — never hand-edit

Fonts and splash are **generated**; regenerate instead of editing the tables
(commands in the [regen-fonts](.claude/skills/regen-fonts/SKILL.md) /
[regen-splash](.claude/skills/regen-splash/SKILL.md) skills):

- `font_data.cpp` (TFT, anti-aliased 6×8 from TTF) and `font_oled.cpp` (OLED,
  crisp 1bpp 5×7). Cell geometry (6×8) is fixed in both `fontgen.cpp` and
  `font.h` and must stay in sync — `menu.cpp` layout maths depends on it.
- `splash_anim.cpp` / `splash_oled.cpp`, baked from `docs/img/frog-anim.svg` —
  the **single source of truth**, also embedded by the web page.
- Generated/vendored files are listed in `.clang-format-ignore`.

## Don'ts

- Don't add features that aren't on the user's TODO list ([TODO.md](TODO.md)).
- Don't add WiFi, mDNS, or any extra network surface — design rule, ArtNet UDP is the only one.
- Don't write to NVS from `render_task` or ISRs — only from `ui_task`.
- Don't put a frame-buffer-sized allocation in SRAM — that lives in PSRAM.
- Don't sleep in ISRs, don't `printf` in ISRs, don't grab mutexes in ISRs.
- Don't add a comment block to "summarize" what a function does when the name + types already say it.
