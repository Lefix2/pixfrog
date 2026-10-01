# TODO — product roadmap

The open backlog, grouped by theme. Per [AGENT.md](AGENT.md), features land
only from this list. Suggested first picks are marked ★. Finished items are
removed; git history and the PRs keep the record (last cleanup: 2026-10-01).

DMX512 output left this firmware (PR #100): it lives in a DMX node firmware
forked at `d4f0f77`. DMX-output work (44 Hz pacing, ArtNzs routing, RDM)
belongs to that fork.

## Bugs & small fixes

- [ ] **`POST /api/global web_enabled:false` silently does nothing** — the
      flag persists but the server keeps running (a handler can't stop its own
      server) and the response has no "applies after reboot" note. Minimum: the
      note; better: defer `web::stop()` to a timer/task.
- [ ] **Factory reset leaves opt-in services running** — web/UART
      `factory-reset` zero `sacn_enabled`/`fpp_remote`/`web_enabled` (and the
      control universe) but sACN/FPP/web keep running until reboot; stop them
      or return the reboot note consistently.
- [ ] **`cmd_fseq` breaks the console OK/ERR convention** — `return 1` on a
      play failure makes esp_console append its own "command returned
      non-zero" line after `ERR`.
- [ ] **`fseq::list_files` typo** — `(ext[0] == '.' || ext[0] == '.')`;
      replace the 5-char test with `strcasecmp(ext, ".fseq")`.
- [ ] **UART console can't get/set `language`** — AGENT.md promises every
      config field; add the key to `cmd_global`.
- [ ] **Stale comments in `config_store`** — `config_store.h` points at a
      `config_get_runtime_snapshot()` that doesn't exist (see *serialize config
      writes*), and `nvs_load_blob`'s doc block sits above
      `fill_default_scenes`.
- [ ] **Scene clock wraps after 49.7 days** — effects run on
      `uint32_t(esp_timer/1000)`; a permanent install sees one jump. Use a
      64-bit phase or wrap on a period the effects are continuous over.
- [ ] **Encoder acceleration feels wrong** — the ×10/×100 multiplier
      (`menu.cpp` `accel_note_rotation`) only resets after 350 ms without a
      detent. Reset the streak on a direction reversal and give ×100 a shorter
      timeout than ×10.
- [ ] **Network silent after a USB-flash reset (seen once, 2026-09-30)** — link
      up + static IP but no ping/HTTP for about a minute; a UART reset fixed it.
      If it comes back, capture EMAC/PHY state and ARP from the PC before
      resetting.

## Concurrency & robustness

- [ ] **Serialize config writes** — `config::set_*` (whole-struct
      read-modify-write + NVS) are called from `ui_task`, the UART console,
      `httpd` and `artnet_rx` (ArtAddress/ArtIpProg) with no lock: concurrent
      writers lose each other's fields, readers can see torn structs. Scene
      list edits `memmove` the bank while `render_task` reads it. Cheapest fix:
      a mutex inside the setters (none is hot-path) and a per-frame copy of the
      scenes being rendered; update AGENT.md's concurrency rule.
- [ ] **Torn 64-bit activity timestamps** — `g_last_activity_us[]` (int64,
      written on core 0, read on core 1) is not atomic on RV32: a reader can
      see mixed halves at a 2³² µs rollover → one spurious failsafe frame.
      Store 32-bit ms or use `std::atomic<int64_t>`.
- [ ] **Versioned NVS blobs** — layouts are told apart by size (the scene
      v1/v2/v3 migration relies on sizes never colliding). Prefix each blob
      with a version byte.
- [ ] **Document the swap-mutex priority inversion** — `g_uni_swap_mux` is
      shared by `render_task` (prio 20) and the receivers (prio 10); bounded by
      one 512-byte memcpy, fine, but say so in ARCHITECTURE.md §6.

## Security

The web password stays optional by design: pixfrog targets private show
networks, and a lighting operator may run without one. Items here harden what
a password protects once set; none of them may make it mandatory.

- [ ] **Strengthen the password hash** — salted single-round SHA-256 is
      brute-forceable offline if the NVS blob leaks (via a coredump). A few
      thousand iterations cost nothing on the P4; re-hash on the next set.
- [ ] **Document cleartext Basic auth** — one sentence in README/AGENT.md:
      the password crosses the LAN in clear (no TLS).

## Show control & scenes

- [ ] **Scene vs network priority policy** — a playing scene overrides the
      network until stopped, and nothing tells the desk. Add a per-box policy:
      `override` (today) / `yield` (the scene is an idle look that stops when
      DMX arrives on its outputs, resumes on failsafe). Surface it in the
      ArtPollReply NodeReport and on HOME.
- [ ] **Per-scene fade time** — the fade is global (`scene_fade_ms`, desk
      overridable); a per-scene value needs a v4 Scene layout.
- [ ] **Shared scene clock across boxes** — each box animates from its own
      `esp_timer` phase, so neighbours drift. Share a phase origin (FPP
      MultiSync, ArtTrigger timestamp, or a small broadcast).
- [ ] **OSC input** — opt-in UDP OSC receiver (QLab, TouchDesigner):
      `/pixfrog/scene N`, `/pixfrog/master f`, `/pixfrog/blackout`,
      `/pixfrog/fseq/play name`.
- [ ] **GPIO trigger inputs** — debounced dry-contact inputs mapped to scene /
      sequence / stop / blackout (GPIO21 is free); museum and escape-room
      staple.
- [ ] **Time-of-day scheduling** — start/stop scenes or playlists on a
      schedule; needs opt-in SNTP and/or an RTC on the shield.

## FSEQ / standalone playback

- [ ] ★ **Loop / playlist / autostart** — loop a file, chain several with
      per-item repeat, start a file or playlist at boot (sibling of
      `boot_scene`). Today a sequence plays once and stops.
- [ ] **Configurable FSEQ start universe** — `kUniverseBase = 1` is hard-coded
      in `fseq_player.cpp`; expose it or map absolute channels onto the
      channel configs.
- [ ] **Pacing tied to the render clock** — the player paces on FreeRTOS
      ticks, independent of `render_task`: a 40 fps file on a 60 Hz render
      judders. Pick the FSEQ frame by elapsed time in the render loop.
- [ ] **Block handling** — O(n) block lookup + offset recompute per frame, and
      a whole ≤ 2 MB zstd block decompressed at once (can exceed a frame
      period). Precompute offsets, decompress the next block ahead of time.
- [ ] **FPP MultiSync tolerance** — `kToleranceMs = 100` is 4 frames at
      40 fps; slew the pacing clock for small drifts instead of seeking.
- [ ] **Audio with FSEQ** — play the sequence's media file in sync through the
      on-board ES8311 codec (I2C 0x18): a standalone mini-FPP.

## Network & protocols

- [ ] **Unique mDNS hostname** — the fixed `pixfrog` collides on `.local`
      with several boxes; derive `pixfrog-XXXX` from the MAC.
- [ ] **ArtSync / sACN sync mode** — banks are published on the first dirty
      slot, so a channel spanning several universes can show two source frames
      at once. Once a sync is seen, hold publication until the next one
      (free-run after 4 s without, Art-Net 4) and honour the E1.31 sync
      address. `g_sync_pending` is already there, unused.
- [ ] **ArtPollReply: one bind per universe** — it advertises 8 ports with the
      global net/subnet and each channel's first universe only; desks (MADRIX,
      xLights) should see every mapped universe with its own Net/SubNet (and
      the control universe).
- [ ] **sACN sequence check and per-source priority** — no E1.31 §6.7.2
      out-of-order discard, and a source lowering its own priority is rejected
      for 2.5 s by the per-universe gate; track priority per CID.
- [ ] **sACN multicast on a real LAN** — only unicast was validated (bench
      behind a NAT). Drive the board from xLights or a desk on the same LAN,
      incl. a universe re-config (5 s join refresh) and the control universe.
- [ ] **Inter-controller frame sync (PTP)** — genlock several boxes (±µs) where
      no ArtSync master exists. The P4 EMAC does IEEE 1588v2 hardware
      timestamping and IDF ships the API + a master/slave example; feed the
      disciplined time into `render_task`'s `next_frame_us` deadline. Opt-in
      (`ptp_enabled`), free-run fallback, defined precedence vs ArtSync.
      Retire first on ≥ 2 boards: the known RMII_CLK ⇄ PSRAM issue at 200 MHz,
      and the IP101 clock prerequisites.
- [ ] **Art-Net opcodes** — `ArtCommand` (mirror the console `key=value`),
      `ArtDiagData` (diagnostics to subscribed controllers); `ArtFirmwareMaster`
      only for completeness (web OTA is better).

## Output & performance

- [ ] **Current limiter (ABL)** — per-channel amp budget (mA at full, PSU
      limit) scaling the frame down when the sum exceeds it.
- [ ] **Adaptive PCLK / sample density** — NRZ is encoded at 16 MHz with 20
      samples per bit where 3-4 at ~3.2 MHz suffice: frame buffers and
      PSRAM/DMA bandwidth are 5-6× larger than needed. Choose the density per
      frame from the protocol mix (clocked protocols keep 16 MHz).
- [ ] **Buffer sizes for longer lines** — at low refresh the wire allows more
      than the 1024 px cap (WS2815 @20 Hz ≈ 1600 px), but every buffer is
      sized from `kMaxPixelsPerChannel`: SRAM pixel buffers, the three PSRAM
      frame buffers (3 × 2.6 MB), the universe pool (64, capped by the uint64
      dirty mask), sACN joins, the UI editors. Pick a target (e.g. 2048 px)
      first; shrinks a lot with adaptive density.
- [ ] **16-output NRZ mode** — with no clocked channel, reuse the 8 CLOCK bus
      bits as 8 more NRZ outputs (16 × 512 px @60 Hz). Needs adaptive density
      and shield support.
- [ ] **Split the encode across both cores** — decode + effects + encode run on
      core 1 while core 0 idles; encode two halves in parallel if heavy effects
      × 8 × 1024 px get tight.

## Web UI

- [ ] **Push live status** — every tab polls `/api/status` each second; a
      WebSocket or SSE push scales better with several clients.
- [ ] **Live output preview** — low-resolution read-back of the pixel front
      buffers (`pixr`-like endpoint) drawn as strips, for remote commissioning.
- [ ] **Expose `persist_ok` in `/api/status`** — degraded-NVS mode is only
      visible over UART; the SPA should warn when settings no longer persist.
- [ ] **Backup filename** — include the date and `short_name` in the
      `Content-Disposition` name (several boxes collide on
      `pixfrog-config.json`).
- [ ] **CSS classes instead of inline styles** — the UI is inline styles in
      HTML and JS templates (a theme change was a 660-line diff); move the look
      to classes + CSS variables, still one embedded file.

## Hardware & bench

- [ ] **Bench SD card for FSEQ** — `hw_validate fseq` needs a microSD in the
      board (every other validator passes on the bench).
- [ ] **Scope the first LED** — confirm on the wire (Saleae on GPIO2 +
      `nrz_decode.py`) that pixel 1 is emitted; the bench strip's first LED is
      sacrificial (3.3 V data without a level shifter).
- [ ] **Multi-board validation** — the aggregated multi-node web UI and
      PTP/sync work need ≥ 2 boards on one LAN.
- [ ] **Physical DMX input** — RS-485 receive on the shield to drive the box
      without a network (hardware work).

## Tests & tooling

- [ ] **Spike: IDF `linux` target for integration tests** — FreeRTOS POSIX,
      real NVS on emulated flash, lwIP on host sockets; the portable components
      could run on the real IDF implementations. Unknown: `esp_http_server` has
      no linux support; cost: slower builds, no fake clock.
- [ ] **Split `menu.cpp` (3.4 k lines) and `web_config.cpp` (2.2 k lines)** per
      node / resource, and share the JSON field parsers between the `POST`
      handlers and `restore_*`.

## Documentation & website

The site (`.github/pages`, published by `pages.yml`) still shows the
pre-September UI and feature set. *(owner)* items need the hardware or CAD.

- [ ] ★ **Reproducible web UI screenshots** — `tools/screenshots/`: the host API
      server (`pixfrog_api_host`) with a demo config + headless Chrome, one
      command → PNGs, so every UI change refreshes the visuals.
- [ ] ★ **New visuals** — dashboard (SHOW card), channels, scenes (list,
      editor, PLAY ON), DMX control screen; TFT screens from the emulator
      goldens (`tools/emulator/crawl.py`).
- [ ] **Effects gallery** — one space-time strip per scene effect (x = pixel,
      y = time), rendered on the host from `fill_scene_pattern`.
- [ ] **Content pass** — site home/about/docs pages (FR + EN) for what shipped
      since the last refresh: 30 scenes / 11 effects / palettes, the new web
      UI, refresh 20-120 Hz, pixel gaps, show control and zones.
- [ ] *(owner)* **Mechanical renders, board renders, photos** — rack 1U and
      satellite renders, `pixfrog-shield.png` / `pixfrog-sat.png` from current
      KiCad, rack photos, site hero and `og-cover.png`, in-use shots.
