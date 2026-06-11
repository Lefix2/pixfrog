# TODO — product roadmap

Improvement backlog, grouped by value. Per [AGENT.md](AGENT.md), features land
only from this list. Items are unordered within a section; suggested first
picks are marked ★.

## Protocol / network

- [x] **sACN (E1.31) receiver** — `components/sacn`, opt-in via
      `sacn_enabled`. Multicast joins per configured universe (5 s refresh),
      per-universe priority gate with 2.5 s source timeout, E1.31 sync →
      frame sync. Equal-priority multi-source merge still pending (see next
      item). Unicast validated end-to-end on hardware (PR #24).
- [ ] **sACN multicast test on a real LAN** — IGMP joins are untestable from
      the WSL2 NAT (only unicast was validated). Drive the board from
      xLights or a console on the apartment LAN and confirm
      `sacn_packets_rx` climbs with multicast-addressed universes
      (239.255.x.y), including after a universe re-config (5 s join
      refresh).
- [ ] **2-source merge (HTP/LTP)** — today two senders on one universe fight
      (last write wins). Standard: track up to 2 sources per universe, merge
      HTP (dimmers) or LTP, drop a source after ~10 s silence. Mode is
      signalled in ArtPollReply and set via ArtAddress.
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
reboot applies). Validated + counted in `stats artnet_ctrl_rx` but not yet
consumed: `ArtNzs` (payload not routed — alternate-start-code storage and
DMX512 encoder interleaving needed), `ArtTrigger` (waiting on scenes),
`ArtCommand`, `ArtTimeCode`. Remaining candidates:

| Opcode | Value | What it brings |
|---|---|---|
| `ArtNzs` routing | 0x5100 | Store alternate-start-code frames + emit them on DMX512 channels (encoder interleaving). |
| `ArtTrigger` consumer | 0x9900 | Fire standalone scenes once they exist. |
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
- [ ] **Config export/import** — JSON dump/restore via the web UI: backup
      before changes, clone one box to the next.
- [ ] **Standalone scenes** — a few built-in effects (solid colour, chase,
      rainbow) playable without a console; doubles as install validation.
      Hook for ArtTrigger.

## UX / commissioning

- [ ] **Channel identify** — from the channel menu, flash the selected strip
      (extends the pixel-count preview shipped in #21) to physically match
      strip ↔ channel.
- [ ] **Per-channel gamma / white balance** — lookup table applied at encode
      time (already a per-pixel pass, near-zero cost).

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

- [ ] **Output buffers** — bus outputs are raw 3.3 V GPIO; for WS2815 (12 V
      strips) over real cable runs, a 74HCT245 per bank would make levels
      robust. The VDD_IO_5/LDO VO4 incident (#22) shows how little margin
      there is today.
