# TODO — product roadmap

The open backlog, grouped by theme. Per [AGENT.md](AGENT.md), features land
only from this list. Suggested first picks are marked ★. Finished items are
removed; git history and the PRs keep the record (last cleanup: 2026-10-09).

DMX512 output left this firmware (PR #100): it lives in a DMX node firmware
forked at `d4f0f77`. DMX-output work (44 Hz pacing, ArtNzs routing, RDM)
belongs to that fork.

## Bugs & small fixes

- [ ] **Network silent after a USB-flash reset (seen once, 2026-09-30)** — link
      up + static IP but no ping/HTTP for about a minute; a UART reset fixed it.
      If it comes back, capture EMAC/PHY state and ARP from the PC before
      resetting.

## Show control & scenes

- [ ] **Scene vs network priority policy** — a playing scene overrides the
      network until stopped, and nothing tells the desk. Add a per-box policy:
      `override` (today) / `yield` (the scene is an idle look that stops when
      DMX arrives on its outputs, resumes on failsafe). Surface it in the
      ArtPollReply NodeReport and on HOME.
- [ ] **Per-scene fade time** — the fade is global (`scene_fade_ms`, desk
      overridable); a per-scene value takes the bytes reserved for it in
      `config::Scene`.
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

## Pro lighting control

Feedback from a lighting operator (GrandMA): drive the box like a conventional
fixture, not only as a pixel-mapped node. In landing order — each item builds
on the one before.

- [ ] ★ **GDTF export of a profile** — a native fixture file for GrandMA3, next
      to the OFL one.
- [ ] **Scene parts aimed at fixture groups** — on a group a scene plays its
      first part only; let each part target a group (across outputs, in the
      group's order) next to the parts on outputs, so one scene holds several
      effects on several groups.
- [ ] **Effects on the device menu** — fixtures and scenes are edited on the
      TFT / OLED; the effect bank (generator, colours, speed, phaser, Block /
      Groups / Wings) is still web / console only. A scene's part picks its
      effect by name, so a look cannot be tuned from the box.

## FSEQ / standalone playback

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

- [ ] **Adaptive PCLK / sample density** — NRZ is encoded at 16 MHz with 20
      samples per bit where 3-4 at ~3.2 MHz suffice: frame buffers and
      PSRAM/DMA bandwidth are 5-6× larger than needed. Choose the density per
      frame from the protocol mix (clocked protocols keep 16 MHz).
- [ ] **Buffer sizes for longer lines** — at low refresh the wire allows more
      than the 1024 px cap (WS2815 @20 Hz ≈ 1600 px), but every buffer is
      sized from `kMaxPixelsPerChannel`: SRAM pixel buffers, the three PSRAM
      frame buffers (3 × 2.6 MB), the universe pool (`kNumUniverses` = 72),
      sACN joins, the UI editors. Pick a target (e.g. 2048 px) first; shrinks a
      lot with adaptive density.

## Hardware & bench

- [ ] **Physical DMX input** — RS-485 receive on the shield to drive the box
      without a network (hardware work).

## Tests & tooling

- [ ] **Spike: IDF `linux` target for integration tests** — FreeRTOS POSIX,
      real NVS on emulated flash, lwIP on host sockets; the portable components
      could run on the real IDF implementations. Unknown: `esp_http_server` has
      no linux support; cost: slower builds, no fake clock.

## Documentation & website

UI captures and the effect sheets are generated (`tools/screenshots`,
`tools/effects_gallery`): re-run them after a UI or effect change. *(owner)*
items need the hardware or CAD.

- [ ] *(owner)* **Mechanical renders, board renders, photos** — rack 1U and
      satellite renders, `pixfrog-shield.png` / `pixfrog-sat.png` from current
      KiCad, rack photos, site hero and `og-cover.png`, in-use shots.
