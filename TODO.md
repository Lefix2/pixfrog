# TODO — product roadmap

Improvement backlog, grouped by value. Per [AGENT.md](AGENT.md), features land
only from this list. Items are unordered within a section; suggested first
picks are marked ★.

DMX512 output was removed (2026-09-30): it moves to a DMX node firmware forked
from this project at `d4f0f77`, the last commit that still has it. DMX-output
items (44 Hz pacing, ArtNzs routing, RDM) belong to that fork.

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
      *Review 2026-09:* the scene list made it worse — `delete_scene` /
      `move_scene` `memmove` the bank while `render_task` may be reading the
      active scene, and the active-index remap (`dmx::scene_list_edited`) is
      not atomic with the edit. Worst case one wrong frame (fields are
      sanitized at render), but the same mutex should cover scene edits, and
      `render_task` should take a per-frame *copy* of the active scene
      (`get_scene` returns a reference into mutable storage).
- [ ] **Torn 64-bit activity timestamps** — `g_last_activity_us[]` (int64,
      written by the receiver tasks on core 0, read by `render_task` on core 1)
      is not atomic on RV32: at every 2³² µs rollover (~71 min) a reader can
      see mixed halves → one spurious failsafe frame. Store 32-bit ms or use
      `std::atomic<int64_t>`.
- [ ] **Document the swap-mutex priority inversion** — `g_uni_swap_mux` is
      taken by `render_task` (prio 20) and the receivers (prio 10); bounded by
      one 512-byte memcpy, acceptable, but say so in ARCHITECTURE.md §6.
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
- [x] **`get_scene` out-of-range** — now returns a blank scene (PR #88,
      variable-length scene list).

## Review 2026-09 — bugs & edge cases

From the September 2026 functional/technical review.

- [x] ★ **Refresh change truncates pixel counts for good** —
      `clamp_pixel_counts()` rewrites `pixel_count` in NVS when the refresh
      rate rises (1024 px @30 Hz → 512 @60 Hz) and nothing restores it when
      going back to 30 Hz. Keep the requested count; clamp only what is
      emitted (and flag the channel over budget in the UIs).
- [ ] ★ **FSEQ playback tears** — `fseq_player` injects through
      `dmx::inject_universe()`, which writes *both* banks while `render_task`
      reads the front one (the function is documented as a bench path). Route
      FSEQ frames through the back bank + dirty mask like network data, and
      publish them atomically per FSEQ frame.
- [ ] **Network silent after a USB-flash reset (seen once)** — 2026-09-30,
      right after `idf.py flash` (esptool hard reset via RTS) the board logged
      link up + static IP but answered neither ping nor HTTP for about a
      minute; a reset through the UART console fixed it. Not reproduced on
      later flashes. If it comes back: capture EMAC/PHY state (`eth` link
      speed/duplex, ARP from the PC side) before resetting.
- [ ] **Encoder acceleration feels wrong** — the ×10 / ×100 step multiplier
      (`menu.cpp` `accel_note_rotation`) only resets after 350 ms without a
      detent, whatever the direction. Reset the streak on a direction
      reversal (overshoot correction must be ×1), and give the ×100 tier a
      shorter timeout than ×10 (a brief pause drops back from ×100).
- [x] **Failsafe "scene" ignores the scene's channel mask** — every lost
      channel plays it (`dmx_manager.cpp` decode path). Honour the mask (or
      document that failsafe uses the scene as a pattern only).
- [ ] **Scene clock wraps after 49.7 days** — effects run on
      `uint32_t(esp_timer/1000)`; a permanent install sees one jump. Use a
      64-bit phase or wrap it on a period the effects are continuous over.
- [ ] **sACN: no sequence-number check** (E1.31 §6.7.2 — discard
      out-of-order packets), and a source that *lowers* its own priority is
      rejected for 2.5 s by the per-universe gate (`sacn_parser.h`
      `gate_accept`): track priority per source CID, not per universe.
- [ ] **FPP MultiSync tolerance is coarse** — `kToleranceMs = 100` is 4
      frames at 40 fps, visible between neighbouring boxes. Slew the pacing
      clock for small drifts instead of seeking, and tighten the threshold.
- [x] **OTA confirmed too early** — `esp_ota_mark_app_valid_cancel_rollback()`
      runs right after the tasks spawn, before a single frame rendered; a
      crash at t+2 s is not covered by rollback. Confirm after ~30 s of
      healthy `render_task` frames with the network up. **Log every
      rollback**: detect it at boot (`esp_ota_get_last_invalid_partition`),
      `ESP_LOGW` the rejected + running versions and the reset reason, keep a
      "last rollback" record in NVS, and show it in the web Diagnostics tab,
      as a dashboard banner until acknowledged, and in the console
      (`version`).

## Review 2026-09 — show control (desk / theatre)

- [ ] ★ **DMX control universe ("personality" mode)** — a configurable
      universe/address whose few slots drive the box from any desk with a
      generic fixture profile: master dimmer, scene number, speed, param,
      colour 1/2 (RGB), strobe. Probably the most valuable missing feature for
      theatre use (few desks can send ArtTrigger).
- [ ] ★ **Grand master + blackout** — global intensity applied at encode
      time (like `brightness`), plus a blackout toggle; reachable from the web
      UI, the TFT, UART, ArtTrigger (a reserved key/subkey) and the control
      universe above.
- [ ] **Scene vs network priority policy** — today a playing scene (and the
      boot scene) overrides the network until stopped, and nothing tells the
      desk. Add a per-box policy: `override` (today) / `yield` (a scene is an
      idle look that stops as soon as DMX arrives on its channels, resumes on
      failsafe). Surface the override in ArtPollReply NodeReport and on the
      HOME screen.
- [ ] **Scene transitions** — crossfade time (per scene or global) when
      switching scenes or starting/stopping one.
- [ ] **Scene zones** — several scenes active at once on disjoint channel
      masks (scene A on outputs 1-4, B on 5-8); today one scene is global.

## Review 2026-09 — standalone installation

- [ ] ★ **FSEQ loop / playlist / autostart** — loop a file, chain several
      (playlist with per-item repeat), start a file or playlist at boot
      (sibling of `boot_scene`). Today a sequence plays once and stops.
- [ ] **Configurable FSEQ start universe** — `kUniverseBase = 1` is
      hard-coded in `fseq_player.cpp`; expose it (or map FSEQ absolute
      channels onto the channel configs).
- [ ] **FSEQ pacing tied to the render clock** — the player paces on FreeRTOS
      ticks, independent of `render_task`: a 40 fps file on a 60 Hz render
      judders. Let the render loop pick the FSEQ frame by elapsed time (or
      follow the file's step time, see the refresh-rate item).
- [ ] **FSEQ block handling** — O(n) block lookup + offset recompute per
      frame, and a whole ≤2 MB zstd block decompressed in one go (can exceed a
      frame period → catch-up burst). Precompute offsets, decompress the next
      block ahead of time.
- [ ] **GPIO trigger inputs** — dry-contact / button inputs mapped to
      scene/sequence/stop/blackout (GPIO21 is free since PR #25), with
      debounce; museum and escape-room staple.
- [ ] **Time-of-day scheduling** — start/stop scenes or playlists on a
      schedule. Needs trustworthy time: opt-in SNTP, and/or an RTC on the
      shield (none on the dev kit).
- [ ] **DHCP-timeout fallback address** — with no DHCP server the box stays
      at 0.0.0.0 forever. Fall back to link-local 169.254.x.x (or Art-Net
      2.x.x.x) after a timeout so "plug a laptop in and configure" works.

## Review 2026-09 — touring / events

- [x] ★ **Pixel gaps anywhere (null / dead pixels)** — up to 8 physical gaps
      per channel (position, length): leading null pixels (sacrificial
      level-shift LED — our bench strip needs one), and dead pixels mid-line
      (a repeater or injector carrying an LED chip). DMX data fills the live
      pixels only; gaps stay black; universe usage is unchanged, the pixel
      budget counts physical pixels. Applied in the encoder after grouping and
      invert (a gap is wiring, it must not move when the direction flips).
      Editable from the web UI, the console (`ch N gaps 0:1,300:2`) and the
      TFT (per-channel "Dead pixels" submenu); the pixel-count ruler shows
      the gaps in their own colour. ChannelConfig grows, zero-fill = no gap.
- [x] ★ **Any refresh rate 20..120 Hz, step 1** — only 30|60 Hz today (web,
      console, TFT editor steps by 30). xLights commonly exports 20/40 fps
      (60 Hz rendering then judders) and Europe needs 25/50 Hz for
      camera-friendly output. The code is already rate-generic (period =
      1e6/rate, budgets); only the validators and the
      editors change, and the pixel budget follows. Measured on the board
      (2026-09-30): encode ≈ 23 µs per pixel row for 8 WS2815 outputs —
      8 × 235 px (the 120 Hz budget) encodes in ≈ 5.5 ms for an 8.3 ms
      period, so 120 Hz holds at ~75 % of core 1. The limit above 60 Hz is the
      wire: WS281x ≈ 235 px, SK6812 ≈ 190 px, APA102 @4 MHz ≈ 900 px @120 Hz.
- [ ] **Current limiter (ABL)** — per-channel amp budget (mA per channel at
      full, PSU limit) scaling the frame down when the sum exceeds it. Safety
      for 5 V / 12 V supplies.
- [ ] **OSC input** — opt-in UDP OSC receiver (QLab, TouchDesigner):
      `/pixfrog/scene N`, `/pixfrog/master f`, `/pixfrog/blackout`,
      `/pixfrog/fseq/play name`. The HTTP API works but is awkward from those
      tools.
- [ ] **Live output preview in the web UI** — low-resolution read-back of
      the pixel front buffers (`pixr`-like endpoint) drawn as strips, for
      remote commissioning.

## Review 2026-09 — bigger features

- [ ] **Audio with FSEQ** — play the sequence's media file in sync through
      the on-board ES8311 codec (I2C 0x18): a standalone mini-FPP.
- [ ] **Physical DMX input** — RS-485 receive on the shield to be driven
      without a network (hardware work).

## Review 2026-09 — performance & architecture

- [ ] **Adaptive PCLK / sample density** — NRZ is encoded at 16 MHz with 20
      samples per bit; 3-4 samples at ~3.2 MHz are enough. Frame buffers and
      PSRAM/DMA bandwidth are 5-6× larger than needed. It does not move the
      physical wire limit (≈512 px @60 Hz per 800 kbps output) but frees CPU
      and bandwidth. Choose PCLK/density per frame from the protocol mix
      (clocked protocols keep 16 MHz).
- [ ] **Buffer sizes for longer lines** — at low refresh the wire allows more
      than the 1024 px cap (WS2815 @20 Hz ≈ 1600 px), but every buffer is
      sized from `led::kMaxPixelsPerChannel` = 1024: SRAM pixel buffers
      (8 × 2 × 4 KB, `dmx::kMaxBytesPerChan`), the three PSRAM frame buffers
      (`kMaxSamplesPerFrame`, sized for the 40-samples/bit worst case: 3 ×
      2.6 MB), the universe pool (`kNumUniverses` = 64, capped by the uint64
      dirty mask — 8 × 2048 px RGBW needs 128 universes), sACN joins and the
      UI editors. Decide the target (e.g. 2048 px/channel), then: pixel buffers
      to PSRAM or larger SRAM, a wider dirty mask, bigger pool, FB budget
      (shrinks a lot with adaptive sample density, below).
- [ ] **16-output NRZ mode** — with no clocked channel configured, reuse the
      8 CLOCK bus bits as 8 more NRZ DATA outputs (16 × 512 px @60 Hz).
      Depends on the adaptive density above and on the shield (buffers,
      connectors).
- [ ] **Split the encode across both cores** — decode + effects + encode all
      run on core 1 while core 0 is mostly idle; encode two halves of the
      sample buffer in parallel if heavy effects × 8 × 1024 px get tight.
- [x] **Serve the SPA gzipped with cache headers** — 226 KB sent raw on every
      load (`handle_root`), no `Cache-Control`/`ETag`, and httpd is
      single-threaded (a page load stalls API calls). gzip at build (~60 KB)
      + ETag = firmware version.
- [ ] **Push live status** — every tab polls `/api/status` each second; a
      WebSocket (or SSE) push scales better with several clients.
- [ ] **Versioned NVS blobs** — layouts are told apart by blob size (the
      scene v1/v2/v3 migration relies on sizes never colliding). Prefix each
      blob with a version byte.
- [ ] **ArtPollReply: one bind per universe** — it advertises 8 ports with
      the *global* net/subnet + each channel's `universe_start` low nibble:
      wrong for channels on another net/subnet, and a channel's 2nd..nth
      universes are never advertised (desk auto-discovery — MADRIX, xLights —
      sees 8 universes instead of up to 48). Emit one bind per mapped
      universe with its own Net/SubNet.
- [ ] **ArtSync / sACN sync mode** — banks are published on the first dirty
      slot of a frame, so a channel spanning several universes can show two
      source frames at once. Once a sync is seen, hold bank publication until
      the next sync (revert to free-run after 4 s without one, Art-Net 4);
      honour the E1.31 sync address. `g_sync_pending` already exists and is
      dead state.

## Review 2026-09 — refactors & tests

- [ ] **Spike: IDF `linux` target for integration tests** — IDF v5.5 builds
      FreeRTOS (POSIX port), real NVS on emulated flash, esp_timer, esp_event,
      lwIP (host sockets), mbedtls, log for the host. The portable components
      (dmx_manager, config_store, artnet, sacn, web_config) could then run on
      the real IDF implementations with a real UDP/TCP stack. Unknown:
      `esp_http_server` declares no linux support. Cost: slower docker builds,
      no fake clock. Evaluate after the hand-written shim harness (QEMU is not
      an option: Espressif's QEMU has no ESP32-P4 model).

- [ ] **Split `menu.cpp` (3.1 k lines)** per menu node, and
      **`web_config.cpp` (1.7 k lines)** per resource (global, channel,
      scenes, fseq, system/OTA); finish sharing the JSON field parsers between
      `POST` handlers and `restore_*`.
- [ ] **SPA: CSS classes instead of inline styles** — the whole UI is inline
      styles inside HTML and JS string templates (the site-style restyle was a
      660-line diff for a theme change). Keep a single embedded file, move the
      look to classes + CSS variables.
- [ ] **Web API/SPA scenario tests in CI** — the mock API + headless Chrome
      harness used for PR #87/#88 (add/delete/move scene, colour reorder, save
      bodies) as a CI job; today neither the REST handlers nor the SPA logic
      have tests.
- [ ] **FSEQ → banks → render integration test** on the host (fake SD file,
      assert decoded pixels), covering the tearing fix above.

## Documentation & website refresh

The site (`.github/pages`, published by `pages.yml` from `docs/img`) and the
docs still show the pre-September UI, hardware and feature set. Items marked
*(owner)* need the hardware or the CAD files; the rest can be generated.

- [ ] ★ **Reproducible web UI screenshots** — commit the mock API + headless
      Chrome harness used for PR #87/#88 as `tools/screenshots/` (mock
      `/api/*` with a realistic demo config, one command → PNGs), so every UI
      change can refresh the visuals instead of hand-made captures.
- [ ] ★ **New web UI visuals** — regenerate `web-dashboard.png` and
      `web-channels.png` in the site-aligned theme (PR #87) and add the scenes
      screen (list + editor, effect dropdown, palette chips — PR #88); used by
      the site home (`index-*.html` media grid) and README.
- [ ] **TFT visuals** — refresh `ui-home.png` from the emulator (headless
      stdin protocol) and add the scene list / playback screens.
- [ ] **Effects gallery** — one space-time strip per scene effect (x = pixel,
      y = time), rendered on the host from `fill_scene_pattern` (the harness
      used to tune fire in PR #88), for the README and a site section on
      standalone scenes.
- [ ] *(owner)* **Up-to-date mechanical renders** — pixfrog_rack 1U enclosure
      (Fusion 360: chassis, UI holder, PSU, XLR) and the pixfrog_satellite
      mechanics once out of WIP; replace the renders in `docs/HARDWARE.md` and
      on the site.
- [ ] *(owner)* **Board renders** — re-export `pixfrog-shield.png` and
      `pixfrog-sat.png` from the current KiCad revisions.
- [ ] *(owner)* **Photos** — assembled rack (`rack-full/open/closeup.jpg`),
      the site hero photo and the `og-cover.png` social card, plus in-use
      shots (lit strips running scenes/FSEQ, satellite on a long run).
- [ ] **Content pass** — README feature list, site home/about/docs pages and
      `docs/*.md` for what shipped since the last refresh: 30 scenes / 11
      effects / palettes / strobe, the new web UI, scene REST endpoints,
      NVS scene bank (ARCHITECTURE §5 memory budget), then each Review 2026-09
      feature as it lands (refresh rates, pixel gaps, control universe, ...).
      FR and EN pages together.

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

- [ ] **Shared scene clock** — every box animates the same scene with its own
      `esp_timer` phase, so neighbouring boxes drift visibly. Share a phase
      origin (FPP MultiSync, ArtTrigger timestamp, or a tiny broadcast) so the
      same scene lines up across boxes.

## ArtNet opcodes not yet handled

Handled: `ArtDmx`, `ArtPoll`, `ArtPollReply` (emitted), `ArtSync`,
`ArtAddress` (names/net/subnet/SwOut applied + reply), `ArtIpProg` (+ reply;
reboot applies), `ArtTrigger` (global KeyShow plays/stops the standalone
scenes), `ArtTimeCode` (slaves a running FSEQ playback to the desk clock,
100 ms drift tolerance). Validated + counted in `stats artnet_ctrl_rx` but
not yet consumed: `ArtNzs` (payload not routed — no DMX output here; the DMX
node firmware owns it), `ArtCommand`.
Remaining candidates:

| Opcode | Value | What it brings |
|---|---|---|
| `ArtCommand` consumer | 0x2400 | Mirror the UART console (`key=value`). |
| `ArtDiagData` | 0x2300 | Emit diagnostics to subscribed controllers (we'd be a sender; ArtPoll already tells us who wants them). |
| `ArtFirmwareMaster/Reply` | 0xF200 / 0xF300 | OTA via ArtNet — prefer web OTA; note for completeness. |
