# TODO — product roadmap

Improvement backlog, grouped by value. Per [AGENT.md](AGENT.md), features land
only from this list. Items are unordered within a section; suggested first
picks are marked ★.

## Protocol / network

- [ ] **sACN (E1.31) receiver** — the biggest gap vs. commercial controllers
      (xLights, consoles default to it). Second UDP receiver feeding the same
      universe pool; sACN per-universe priority; multicast (IGMP join per
      configured universe).
- [ ] **2-source merge (HTP/LTP)** — today two senders on one universe fight
      (last write wins). Standard: track up to 2 sources per universe, merge
      HTP (dimmers) or LTP, drop a source after ~10 s silence. Mode is
      signalled in ArtPollReply and set via ArtAddress.
- [ ] ★ **Failsafe on signal loss** — configurable behaviour when ArtNet stops:
      hold last look / blackout / fallback scene, after a configurable
      timeout. Today the strips hold the last frame forever.

## ArtNet opcodes not yet handled

Currently handled: `ArtDmx` (0x5000), `ArtPoll` (0x2000), `ArtPollReply`
(0x2100, emitted), `ArtSync` (0x5200). Candidates, most useful first:

| Opcode | Value | What it brings |
|---|---|---|
| `ArtAddress` | 0x6000 | Remote config from the lighting desk: net/subnet, universe swap-in, short/long name, merge mode, indicator state. Replies with ArtPollReply. Pairs with the merge item above. |
| `ArtIpProg` / `ArtIpProgReply` | 0xF800 / 0xF900 | Remote IP/DHCP programming — same fields our web UI exposes, but desk-driven. |
| `ArtNzs` | 0x5100 | Non-zero start-code DMX frames (incl. ArtVlc). Needed for RDM-adjacent and text payloads; cheap to parse, route to DMX512 channels only. |
| `ArtTrigger` | 0x9900 | Show macros/triggers — natural hook for the future standalone scenes (trigger scene N). |
| `ArtTimeCode` | 0x9700 | Timecode distribution — only relevant if scenes/effects become time-synced. |
| `ArtCommand` | 0x2400 | Free-text property commands — could mirror the UART console (`key=value`). |
| `ArtDiagData` | 0x2300 | Emit diagnostics to subscribed controllers (we'd be a sender; ArtPoll already tells us who wants them). |
| `ArtTodRequest/TodData/TodControl/Rdm/RdmSub` | 0x8000–0x8400 | RDM over ArtNet — only meaningful for DMX512 output channels, and needs RDM on the wire (driver work). Large. |
| `ArtFirmwareMaster/Reply` | 0xF200 / 0xF300 | OTA via ArtNet — prefer web OTA below; note for completeness. |

## Operations

- [ ] ★ **OTA via web UI** — HTTP server exists; add a firmware-upload endpoint
      (esp_ota API, A/B partitions — partition table needs a second app slot).
      Removes the USB cable from every update.
- [ ] **Web UI auth** — anyone on the LAN can reconfigure and reboot the
      device. A single password (HTTP basic over the LAN is acceptable here)
      stored in GlobalConfig.
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

- [ ] ★ **Emulator in CI** — it broke silently once already (missing
      web_config stub, caught in #21). Add a CI job: build `tools/emulator`
      + headless smoke test of the menu FSM via the stdin agent protocol.
- [ ] **Encoder INT_N dead** (GPIO21 never fires; UI time-polls). Either trace
      the schematic and fix, or remove the dead ISR path and document polling
      as the design.

## Hardware (next board rev)

- [ ] **Output buffers** — bus outputs are raw 3.3 V GPIO; for WS2815 (12 V
      strips) over real cable runs, a 74HCT245 per bank would make levels
      robust. The VDD_IO_5/LDO VO4 incident (#22) shows how little margin
      there is today.
