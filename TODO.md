# TODO — product roadmap

Improvement backlog, grouped by value. Per [AGENT.md](AGENT.md), features land
only from this list. Items are unordered within a section; suggested first
picks are marked ★.

## Protocol / network

- [x] **sACN (E1.31) receiver** — `components/sacn`, opt-in via
      `sacn_enabled`. Multicast joins per configured universe (5 s refresh),
      per-universe priority gate with 2.5 s source timeout, E1.31 sync →
      frame sync. Equal-priority sources now go through the shared 2-source
      merge (see next item). Unicast validated end-to-end on hardware (PR #24).
- [ ] **sACN multicast test on a real LAN** — IGMP joins are untestable from
      behind a NAT (only unicast was validated). Drive the board from
      xLights or a console on the same test LAN and confirm
      `sacn_packets_rx` climbs with multicast-addressed universes
      (239.255.x.y), including after a universe re-config (5 s join
      refresh).
- [x] **2-source merge (HTP/LTP)** — shared merge engine in `dmx_manager`
      (`merge_ingest` in dmx_logic.h, host-tested): up to 2 sources per
      universe keyed by sender IP (ArtDmx) / CID hash (sACN), per-source
      staging frames in PSRAM, HTP = per-slot max, LTP = last frame wins.
      Source dropped after 10 s silence (Art-Net) / 2.5 s (E1.31); a third
      sender is ignored. Mode is node-wide (`GlobalConfig::merge_mode`,
      default HTP), set via ArtAddress AcMergeLtp*/AcMergeHtp*/AcCancelMerge,
      UART `global merge_mode`, or the web API; reported in ArtPollReply
      GoodOutputA bits 1+3 and `chstat merge=`. sACN priority takeover
      resets the universe's merge so an outranked source can't blend in.
- [x] **ArtTimeCode → FSEQ sync** — `fseq::seek_ms/position_ms/duration_ms`
      (atomic seek consumed at frame boundaries, pacing clock re-anchored);
      the ArtTimeCode handler slaves a *running* playback to the desk clock,
      re-seeking only beyond 100 ms drift. Never auto-starts playback.
      `fseq seek <ms>` on the console for manual control.
- [x] **FPP MultiSync remote** — `components/fpp_sync`, opt-in via
      `fpp_remote` (console/web/menu, zero-fill default off). UDP 32320 +
      multicast 239.70.80.80: master START/STOP/SYNC drive the local FSEQ
      player; SYNC hot-joins a show already running on the master and
      corrects drift > 100 ms. Wire parser host-tested.
- [x] **.fseq upload over the web** — `POST /api/fseq/upload?name=` streams
      the body to the SD card (.part + rename, never a truncated .fseq
      visible), SPA upload card with progress bar. hw_validate
      `validate_fseq.py` uploads its own test sequence, so the suite only
      needs an empty FAT card.
- [x] **Failsafe on signal loss** — global mode (hold / blackout / solid
      colour) + timeout (0 = off), triggered per channel from its activity
      timestamp; never-active channels stay dark; sACN stream_terminated
      expires immediately (E1.31); DMX512 channels degrade colour→blackout.
      Surfaced in menu (FSafe/FSafeS), web UI (colour picker), UART
      (`failsafe_mode|timeout_s|color`), chstat + HOME indicator. Validated
      10/10 on hardware.

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
| `ArtFirmwareMaster/Reply` | 0xF200 / 0xF300 | OTA via ArtNet — prefer web OTA below; note for completeness. |

## Operations

- [x] **OTA via web UI** — `POST /api/ota` (raw .bin → inactive slot,
      esp_ota_end validation, reboot) + upload card with progress bar in the
      SPA. A/B partition table (ota_0/ota_1 7 MB + otadata; nvs kept at its
      old offset so config survived the one-time USB reflash). Rollback:
      BOOTLOADER_APP_ROLLBACK_ENABLE + confirmation at boot-complete — a
      crashing OTA image reverts on next reset. Validated on hardware:
      ota_0 → ota_1, "OTA image confirmed" in the boot log.
- [x] **Web UI auth** — HTTP Basic on every mutating endpoint (config,
      channel, OTA, reboot, factory-reset); GETs stay open. Disabled by
      default: setting an admin password (SPA, `/api/global`, or UART
      `global web_password <pwd|->`) enables it. Salted SHA-256 in
      GlobalConfig, constant-time compare, 500 ms flat delay on failure;
      UART is the recovery channel.
- [x] **Config export/import** — `GET /api/backup` (download, no password
      hash) / `POST /api/restore` (best-effort apply: global + channels +
      scenes), Backup card in the web UI. Round-trip validated on hardware.
- [x] **Live status in the web UI** — `GET /api/status` (heap, fps, uptime,
      rx counters, per-channel active/failsafe, scene + FSEQ position),
      polled every 3 s by the SPA: header live line, green/orange dots on
      the channel tabs, FSEQ progress.
- [x] **Coredump in flash** — 64 KB `coredump` partition appended after
      ota_1 (ONE USB reflash of the partition table; field tables without
      it keep working, dumps just aren't stored),
      `ESP_COREDUMP_ENABLE_TO_FLASH` (ELF), `GET /api/coredump` downloads,
      `DELETE /api/coredump` erases (auth), Crash dump card in the SPA.
- [x] **mDNS `pixfrog.local`** — espressif/mdns managed component,
      advertised (`_http._tcp` + hostname) only while `web_enabled`; no
      extra opt-in flag. Instance name = ArtNet short name.
- [x] **Standalone scenes** — 8 parametric slots (solid / chase / rainbow,
      colour, speed, param, per-channel mask) persisted in NVS. Manual-stop
      priority over network traffic. Triggers: Scenes menu, web tab, UART
      `scene` command, `boot_scene`, ArtTrigger KeyShow (SubKey 1..8 / 0),
      failsafe mode "scene". Validated 12/12 on hardware.
- [x] **FSEQ player from microSD** — the xLights ecosystem standard for
      recorded shows (v2, zstd-compressed frames; played by Falcon/FPP/
      ESPixelStick). Feasible: devkit microSD on GPIO39-44 (no LED-bus
      conflict) and its power rail is the LDO VO4 we already program;
      P4 SDMMC 4-bit has ample bandwidth. Big piece: SDMMC driver + FAT +
      FSEQ parser + playback scheduling.

## UX / commissioning

- [x] **Channel identify** — 2 Hz full-white blink on one strip,
      auto-expiring (default 10 s). Channel menu "Identify", web button,
      UART `identify <ch> [s]`. Top render priority.
- [x] **Per-channel gamma / white balance** — `gamma_x10` (10..40) +
      `wb` RGB per channel, baked into a per-channel `PixelLut` applied in
      every encoder path (NRZ, frame, SPI) before colour-order/brightness;
      identity channels skip the lookups. LUT rebuilt lazily on config
      change (4-byte key compare per frame). NVS tail migration sanitized
      so old configs stay identity. 1291 new encoder assertions.

## Tech debt / infra

- [x] **Emulator in CI** — `ci.yml` job + `tools/emulator/smoke.sh`: SDL2
      build + headless menu-FSM smoke test (navigation, About, EditValue
      commit by long-press, long-press-as-Back). Also in ci-local.sh.
- [x] **Encoder INT_N removed** — analysis: ui_task already loops at ~30 Hz
      for the LED animation + display refresh, so INT gained ≤ one tick of
      latency and cost two extra I2C latch-clear reads per poll (and the
      line never fired on this board). Polling is now the documented design;
      the encoder harness is 4-wire (VCC/GND/SDA/SCL) and GPIO 21 is free.

## Hardware (next board rev)

- [x] **Output buffers** — designed as the **pixfrog shield**
      (`hardware/pixfrog_shield/`, KiCad 10 + JLCPCB production files):
      2× 74HCT245 re-drive the 16 bus lines at 5 V, DIP-selectable series
      termination per line (DATA 249 Ω ↔ ≈34 Ω, CLOCK 33 Ω ↔ ≈18 Ω), one
      5 V TVS clamp per output, 8× JST-XH. Field validation on a long
      cable run still to do once boards arrive.
