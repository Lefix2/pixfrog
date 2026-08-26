# TODO — product roadmap

Improvement backlog, grouped by value. Per [AGENT.md](AGENT.md), features land
only from this list. Items are unordered within a section; suggested first
picks are marked ★.

## Audit 2026-07 — bug fixes

Findings from the July 2026 full-project audit. Small, low-risk, one `fix/` PR.

- [ ] ★ **Web password > 63 chars locks the user out** — `handle_post_global`
      truncates the password to 63 bytes before hashing
      (`web_config.cpp`, `char pwd[64]`), but `check_web_password` hashes the
      full string the browser sends, so a long password set via the SPA never
      authenticates (UART recovery only). Reject > 63 chars with a 400 (and
      surface the limit in the SPA), or size both paths identically. The
      160-byte `Authorization` header buffer imposes a similar silent limit.
- [ ] ★ **Backup/restore drops `fpp_remote` and `lang`** — `restore_global`
      never reads them although `build_global_json` exports both. Also apply
      the sACN/FPP start/stop side effects on restore like `POST /api/global`
      does.
- [ ] **`POST /api/global web_enabled:false` silently does nothing** — the
      flag persists but the server keeps running (a handler can't stop its own
      server) and the response carries no "applies after reboot" note. Minimum:
      add the note; better: defer `web::stop()` to a timer/task.
- [ ] **`cmd_fseq` breaks the console OK/ERR convention** — `return 1` on play
      failure makes esp_console append its own "command returned non-zero"
      line after `ERR` (`control_console.cpp`, cf. the convention comment at
      the top of the file).
- [ ] **`/api/fseq/play` doesn't validate the filename** — it goes straight
      into `snprintf("%s/%s", mount, filename)`; reuse the upload endpoint's
      filter (`/`, `\`, leading `.`). Same handler: single `httpd_req_recv`
      instead of the `read_body` loop truncates a fragmented body.
- [ ] **`fseq::list_files` typo** — `(ext[0] == '.' || ext[0] == '.')`
      duplicates the same test; replace the whole 5-char dance with
      `strcasecmp(ext, ".fseq")`.
- [ ] **Misplaced comment in `config_store.cpp`** — `nvs_load_blob`'s doc
      block sits above `fill_default_scenes` with an orphan line merged in.
- [ ] **`config_store.h` references `config_get_runtime_snapshot()`** which
      doesn't exist. Fix the comment — or implement it (see the concurrency
      item below, which is the real gap).
- [ ] **UART console can't get/set `language`** — AGENT.md promises "every
      config field"; add the key to `cmd_global` (or document the exception).
- [ ] **Stale TODO markers** — "TODO B5" is implemented (test-pattern menu
      node + `cal` command + render hook): drop the 4 markers (`main.cpp`,
      `menu.cpp`, `led_output.h`, `lcd_cam_output.cpp`). `dmx_manager.cpp`'s
      "TODO(v1) hash map" + the "flat allocation N % kNumUniverses" comment
      describe a design that no longer exists (exact 32768-entry LUT — worth a
      line in ARCHITECTURE.md's memory budget: 64 KB .bss). Sweep the orphan
      "Item 7 / B1 / A5" references (they point at a task list that left the
      repo — our own comment rule forbids exactly this).

## Audit 2026-07 — config write concurrency

- [ ] **Serialize config writes** — `config::set_global/set_channel`
      (whole-struct read-modify-write + NVS) are called from four tasks:
      `ui_task`, UART console, `httpd`, and `artnet_rx` (ArtAddress/ArtIpProg).
      No lock: concurrent writers can lose each other's fields; readers
      (`render_task` reads `refresh_rate_hz` each frame) can see torn structs.
      Cheapest fix: a mutex inside `config_store` setters (none are hot-path);
      alternative: funnel writes through one task. Update AGENT.md's rule
      (today it only documents the console/ui_task sharing) and fix the
      `config_get_runtime_snapshot()` comment while at it.
- [ ] **Factory reset leaves opt-in services running** — web/UART
      `factory-reset` zeroes `sacn_enabled`/`fpp_remote`/`web_enabled` but the
      servers keep running until reboot; stop them (or print/return the reboot
      note consistently).

## Audit 2026-07 — security hardening

- [ ] ★ **Passwordless default leaves OTA/factory-reset/reboot open to the
      whole LAN** once the web UI is on. Graduated options: (a) persistent SPA
      banner while no password is set, (b) require setting a password when
      enabling `web_enabled`, (c) gate `/api/ota` + `/api/factory-reset` even
      without a global password (e.g. a token shown on the local display).
- [ ] ★ **Auth on `GET /api/coredump`** — a core dump is raw RAM and may
      contain a cleartext password from a previous Basic-auth request. The
      CORS "GETs stay open" choice doesn't require this one (the multi-node
      dashboard never reads it). Consider `/api/logs` too.
- [ ] **Strengthen the password hash** — salted single-round SHA-256 is
      trivially brute-forceable offline if the NVS blob leaks (precisely via a
      coredump). A few thousand iterations cost ~nothing on the P4 and re-hash
      transparently on the next password set.
- [ ] **Document cleartext Basic auth** — one explicit sentence in
      README/AGENT.md: the password transits in clear on the LAN (no TLS).

## NV3007 display — finish the variant (in progress on this branch)

The 428×142 landscape layout landed (menu/canvas/fonts behind
`CONFIG_PIXFROG_DISPLAY_NV3007`); the build/driver chain is now closed:

- [x] ★ **Panel driver** — `tft_nv3007.cpp`: vendor init sequence
      ("NV3007+IVO" reference, as mirrored in lvgl's `lv_nv3007.c`), GRAM
      column offset 0x0C, software 90° rotation (the reference code never
      programs MADCTL), blocking per-band DMA flush.
      `CONFIG_PIXFROG_NV3007_ROT180` flips modules mounted upside down.
- [x] **Consume `CONFIG_PIXFROG_TFT_WIDTH/HEIGHT`** — `main.cpp` reads them;
      Kconfig now nests the panel choice (ST7789 default / NV3007) under the
      TFT backend so NV3007 builds define both display macros, as the shared
      UI code expects.
- [x] **CI coverage** — `sdkconfig.ci.nv3007` overlay + `nv3007` in the
      `ci.yml` idf-build matrix and in `ci-local.sh`. *(Superseded: NV3007 is
      now the Kconfig default, so it builds with no overlay and the ST7789
      variant moved to `sdkconfig.ci.st7789`.)*
- [x] **Emulator geometry** — `-DPIXFROG_EMU_PANEL=st7789|nv3007` (default
      nv3007); CI + ci-local build and smoke both layouts.
- [x] **Docs** — README display-backend table, AGENT.md module map,
      emulator README.
- [x] **Backlight dimming** — GPIO 45 driven by LEDC (10-bit @ 20 kHz,
      gamma ≈ 2, hardware fade) instead of a static level. `tft_brightness`
      (10..100 %), `tft_idle_dim` (attenuation once idle, 0 = never) and
      `tft_dim_delay_s` (inactivity before dimming, 0 = never, independent of
      `home_timeout_s`) on the encoder DISPLAY node, the console and the web
      System card; the first event after a dim only wakes the screen.
      `docs/HARDWARE.md` §5.1 has the electrical background.
- [ ] ★ **Hardware bring-up** — validate on the real 2.79" module: rotation
      direction (flip with `PIXFROG_NV3007_ROT180` if mirrored), the 0x0C
      GRAM column offset (edge lines), colours/gamma vs the ST7789 look, and
      SPI clock headroom (20 MHz board default; vendor demos run 40+). With
      dimming in: check the 20 kHz backlight PWM does not disturb the SDMMC
      pads sharing VDD_IO_5 (FSEQ playback from SD at a low duty) and that
      low levels stay even across the panel.

## Audit 2026-07 — documentation debt

One `docs/` PR, trivial but prevents real agent/human mistakes:

- [ ] ★ **Host-suite counts + skill** — AGENT.md says "three" (module map) and
      "five" (hard rules) host suites; there are **seven** (ci.yml/README are
      correct). Worse, `.claude/skills/host-tests/SKILL.md` only builds/runs 3
      suites — an agent following it never runs config_store/sacn/fseq/fpp
      before pushing.
- [ ] **AGENT.md module map** — missing `fpp_sync`, `fseq_player`,
      `tools/hw_validate`, `hardware/pixfrog_satellite/`; `components/ui` is
      described as "SSD1306 driver" but also owns ST7789/NV3007 + canvases.
- [ ] **AGENT.md REST endpoint list** — omits `/api/diag`, `/api/logs`,
      `/api/loglevel`, `/api/coredump` (GET/DELETE), `/api/autopatch`,
      `/api/fseq/*`.
- [ ] **`tools/hw_validate/README.md`** — the validator table omits `fseq`
      and `webops` (9 scripts, 7 rows).
- [ ] **`main.cpp` boot comment** — says "LCD_CAM driver"; PARLIO is the
      default.

## Audit 2026-07 — product conveniences

- [ ] **Unique mDNS hostname** — fixed `pixfrog` collides on `.local` with
      several boxes; derive `pixfrog-XXXX` from the MAC so every box is
      addressable by name, not only by IP.
- [ ] **Expose `persist_ok` in `/api/status`** — degraded-NVS mode is visible
      only over UART today; a box that no longer persists its config deserves
      an SPA warning.
- [ ] **Backup filename** — include date + `short_name` in the
      `Content-Disposition` name (`pixfrog-config.json` collides with several
      boxes).
- [ ] **`get_scene` out-of-range** — silently aliases to scene 0
      (`config_store.cpp`); make the clamp explicit.

## Protocol / network

- [ ] **sACN multicast test on a real LAN** — IGMP joins are untestable from
      behind a NAT (only unicast was validated). Drive the board from
      xLights or a console on the same test LAN and confirm
      `sacn_packets_rx` climbs with multicast-addressed universes
      (239.255.x.y), including after a universe re-config (5 s join
      refresh).
- [ ] **Inter-controller frame sync (PTP)** — genlock several pixfrogs to a
      common clock so they switch frames in lockstep (±µs), useful where
      no ArtSync master exists (standalone FSEQ on microSD across cards) or
      where ArtSync UDP jitter shows on large surfaces. HW/IDF are in our
      favour: the ESP32-P4 EMAC does **IEEE 1588v2 hardware timestamping**
      (PTP L2, Annex F) and ESP-IDF ships the full API (enable/disable,
      get/set time, freq adjust, target-time IRQ, PPS) plus a 2-board
      master/slave example. Integration point already exists: feed the
      disciplined PTP time into the drift-corrected `next_frame_us` deadline
      in `render_task` (`main/main.cpp`) and align it on the shared
      `period_us` grid instead of the local `esp_timer` base — a clock-source
      swap, not a hot-path redesign. Needs: `ptp_role` (auto/master/slave) +
      `domain` in `GlobalConfig`; free-run fallback on PTP link loss (like
      the existing `dma_underruns` path); a defined precedence vs ArtSync.
      **Opt-in** (`ptp_enabled`, default off — no PTP module started
      otherwise), per the no-always-on-surface rule.
      Risks to retire first (banc, ≥2 cards, untestable in host/emulator):
      (1) known P4 issue RMII_CLK ⇄ PSRAM — we already run octal PSRAM at
      200 MHz, so confirm timestamping coexists; (2) clock-source prereqs
      from the IDF example with the IP101 PHY. First step: reproduce the IDF
      master/slave example on two pixfrogs and measure the offset.

## Multi-node / fleet

- [x] **Multi-pixfrog discovery + aggregated web UI** — the SPA discovers
      siblings via `GET /api/peers` (mDNS `mdns_query_ptr` on `_http._tcp`,
      filtered on a `product=pixfrog`/`node`/`fw` TXT record, self first,
      5 s cache). One box → UI unchanged. Several boxes → a device bar, an
      **aggregated channel grid** on the dashboard (cross-origin
      `GET /api/status`+`/api/config`, addressed by IP so the
      `pixfrog.local` first-come collision is moot), and per-device
      **config via that box's own UI in an `iframe`** (`/?embed=1` hides the
      device bar in the framed child) — no cross-origin writes. CORS:
      `Access-Control-Allow-Origin: *` on JSON GETs only (simple requests,
      no preflight); writes stay behind Basic auth. No proxy, no extra
      opt-in flag (rides `web_enabled`). Validation multi-cartes (≥2 boards
      sur un LAN) reste à faire sur matériel.

## ArtNet opcodes not yet handled

Handled: `ArtDmx`, `ArtPoll`, `ArtPollReply` (emitted), `ArtSync`,
`ArtAddress` (names/net/subnet/SwOut applied + reply), `ArtIpProg` (+ reply;
reboot applies), `ArtTrigger` (global KeyShow plays/stops the standalone
scenes), `ArtTimeCode` (slaves a running FSEQ playback to the desk clock,
100 ms drift tolerance). Validated + counted in `stats artnet_ctrl_rx` but
not yet consumed: `ArtNzs` (payload not routed — alternate-start-code
storage and DMX512 encoder interleaving needed), `ArtCommand`.
Remaining candidates:

| Opcode | Value | What it brings |
|---|---|---|
| `ArtNzs` routing | 0x5100 | Store alternate-start-code frames + emit them on DMX512 channels (encoder interleaving). |
| `ArtCommand` consumer | 0x2400 | Mirror the UART console (`key=value`). |
| `ArtDiagData` | 0x2300 | Emit diagnostics to subscribed controllers (we'd be a sender; ArtPoll already tells us who wants them). |
| `ArtTodRequest/TodData/TodControl/Rdm/RdmSub` | 0x8000–0x8400 | RDM over ArtNet — only meaningful for DMX512 output channels, and needs RDM on the wire (driver work). Large. |
| `ArtFirmwareMaster/Reply` | 0xF200 / 0xF300 | OTA via ArtNet — prefer web OTA; note for completeness. |
