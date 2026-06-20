# TODO — product roadmap

Improvement backlog, grouped by value. Per [AGENT.md](AGENT.md), features land
only from this list. Items are unordered within a section; suggested first
picks are marked ★.

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
