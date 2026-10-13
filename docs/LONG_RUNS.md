# Long runs — 48 V distribution and a differential pixel link

**Status: study (2026-10).** Nothing here is built yet. This document records
the design agreed after a show where long cables between the controller and
the strips caused voltage drop and corrupted data. It is the base for the
KiCad schematics of a bi-mode shield and a strip node, and for the firmware
work on the controller.

---

## 1. Why the current wiring fails on long runs

Today a channel leaves the shield as **single-ended 5 V logic** (74HCT245,
`hardware/pixfrog_shield`) referenced to GND, and GND also carries the LED
return current when the strip is powered through the cable (XLR4 pin 4).

- **Voltage drop.** The loop resistance of the cable eats the strip supply:
  `ΔV = I × 2 × L × ρ / S` (ρ copper = 0.0172 Ω·mm²/m).
- **Ground shift.** The same drop on the GND conductor lifts the strip's 0 V
  above the controller's. The data threshold moves with it and the 5 V signal
  loses its margin. The satellite (`hardware/pixfrog_satellite`) re-squares
  edges with a Schmitt trigger, but it cannot fix a shifted reference.

Telling them apart on site:

| Symptom | Cause |
|---|---|
| Colours drift to yellow/red towards the end, white is not white, worse at full white | voltage drop |
| Random or flickering pixels, often from one point on, worse when brightness rises | data (ground shift, noise, edges) |

### Distribution voltage

300 W of LEDs at 20 m over 2.5 mm² (0.28 Ω loop):

| Bus | Current | Drop | What the strip sees |
|---|---|---|---|
| 12 V | 25 A | 6.9 V | unusable |
| 24 V | 12.5 A | 3.4 V (14 %) | colour shift |
| **48 V + local DC-DC** | 6.25 A | 1.7 V (3.6 %) | **exactly 24 V**, even with the bus down to ~36 V |

The point is not only a quarter of the current: the node **regulates at the
point of load**, so the strip voltage no longer depends on the cable. 48 V
stays SELV (< 60 V DC), like PoE.

---

## 2. Architecture

```
            48 V PSU
               │
 pixfrog ── XLR (D+/D−) ──┐        ┌── 48V, GND, D+, D− ──┐
 (shield     per output   ├─ inj ──┤                      ├── node ── node ── …
  bi-mode)                │        │                      │    │       │
                          └────────┘                      │  strip   strip
```

- One **differential pair per output**: RS-485 levels, used one way only
  (RS-422 style). The driver on the controller is always on; a node only
  receives and repeats.
- **No telemetry, no return channel.** Reliability comes from a CRC: a node
  drops a corrupted frame and keeps showing the last good one, silently.
- The node turns the link back into the strip's own protocol (WS281x, APA102,
  …) with its own timing, and powers the strip from 48 V through a local buck.
- The 48 V is injected at a power point with its own connector type, **never
  on the shield's XLR**.

### Why a framed protocol rather than NRZ over RS-485

| | NRZ carried as is | pixfrog link (frames + CRC) |
|---|---|---|
| Corrupted data | wrong pixels on stage | frame dropped, last good image kept |
| Clocked strips (APA102) | two pairs (does not fit an XLR3) | one pair; the node makes the clock |
| Line rate | 800 kb/s | 4–8 Mb/s |
| Several strips per pair | no | yes (chain) |
| Synchronisation | implicit | one broadcast latch: all nodes show together |
| Node configuration | strip-specific | none: the frame header names the protocol |

---

## 3. Bi-mode shield

Each output keeps its XLR and can run **classic** (today's single-ended DATA +
CLOCK) or **differential** (D+/D−), chosen per output from the firmware.

```
                ┌─ 74HCT125 (OE̅) ─ 249/34 Ω ─┐
PARLIO DATA ────┤                            ├── XLR pin 2  (DATA  | D+)
PARLIO CLOCK ───┼─ 74HCT125 (OE̅) ─ 33/18 Ω ──┼── XLR pin 3  (CLOCK | D−)
                └─ RS-485 DI ── driver (DE) ─┘
                                 ▲
I2C expander bit n ──────────────┴── same net on both HCT OE̅
```

- **One expander bit per output.** The 74HCT125 OE̅ is active low and the
  RS-485 DE active high, so one net enables one path and tri-states the other,
  with no inverter: 0 = classic, 1 = differential.
- **8 outputs = 8 bits**: a TCA9554-class expander at 0x20. No clash on the
  existing bus (0x3C OLED, 0x36 encoder, 0x18 codec).
- **Pull-downs** hold every output in classic mode from reset until the
  firmware applies the config — today's behaviour.
- **74HCT125 instead of 74HCT245**: the '245 has one OE per 8 lines, so a
  single output cannot be switched. Four '125 (one OE per gate) cover the 16
  lines.
- **The RS-485 driver sits on the pins directly**, without the 249 Ω series
  resistors (they would eat the differential swing). The 120 Ω termination is
  at the far end, in the node.
- **Pinout unchanged** (`HARDWARE.md` §8): 1 = GND, 2 = DATA / D+,
  3 = CLOCK / D−.
- A wrong mode harms nothing: both stay within 0–5 V. A strip on a
  differential output shows garbage; a node on a classic output drops every
  frame on its CRC.
- Trade-off: a differential-only shield could run 16 outputs (one bus line
  each). Bi-mode keeps 8, and keeps every existing install working.

---

## 4. Protection

A TVS absorbs **transients** (ESD, hot-plug spikes, induced surges: mJ for
µs). Against a **sustained** 48 V on a data line it clamps, takes amps from
the PSU, dies within milliseconds — usually **short** — and leaves the line
stuck to ground. The classic path would not survive either: 249 Ω in series
burns ~7 W, 34 Ω passes over 1 A. A PTC in series is too slow (100 ms–1 s) for
a reasonably sized TVS at 48 V, and its resistance degrades the signal.

Three layers, one per threat:

| Threat | Protection |
|---|---|
| ESD, spikes, noise picked up by a long cable | TVS: SM712-class (asymmetric −7/+12 V) on the pair, SMBJ58A on the 48 V |
| 48 V held on D+/D− | **±60 V fault-protected transceivers** (THVD24xx, LTC2862 families), on the shield and in the node |
| Miswiring | **keyed connectors**: 48 V can never land on a data pin; the shield's XLR never carries it |

The 5 V TVS of the current shield would also clip the RS-485 common-mode
range: the differential path gets RS-485-rated TVS instead.

### The 48 V connector — not XLR4

XLR4 looks natural (4 pins: 48 V, GND, D+, D−) but is the wrong choice:

- **Out of rating.** A Neutrik XLR is rated **< 50 V** and 10 A per contact
  (NC4FX); a 48 V PSU trimmed to 50 V sits at the limit, and the 10 A run at
  100 % of the contact.
- **Mates with the 12/24 V XLR4 already in use** (`HARDWARE.md` §8: pin 4 =
  strip VCC). One 48 V cable into an old satellite or a direct strip and the
  strip is gone at once — the most likely accident.
- **No breaking under load.** An XLR is not made to be pulled under current:
  a DC arc at 48 V and 5–10 A does not die out by itself as an AC one does;
  the contacts pit, their resistance rises, they heat. Pin 1 mates first
  (GND), which helps on plug-in only.
- **Thin cables around.** XLR4 leads are often 0.25–0.5 mm² (audio, DMX,
  camera power): one of them in a 10 A run overheats.

Instead, a family that **cannot mate** with anything else on the rig and is
rated for it, e.g. **M12 L-coded** (5 poles, DC up to 63 V and 16 A per pin,
IP67, screw lock, up to 2.5 mm²): 48 V, GND, D+, D−, and the fifth pole free
(or shield). The screw lock also makes an unplug under load deliberate.
Whatever the connector: **switch the PSU off before (un)plugging a segment**.

---

## 5. Strip node

```
48V in ─► eFuse ─► TVS + bulk ─┬─► buck (48→24/12V) ─► strip VCC
          (fuse, reverse,      ├─► buck 5V ─► LDO 3.3V
           soft-start)         └─► 48V out (chain)
D+/D− in ─► TVS ─► 120Ω ─► receiver ±60V ─► RP2040 ─► 5V buffer ─► strip DATA/CLK
                                              └─► driver ±60V ─► D+/D− out (chain)
```

| Block | Parts (families — check datasheets) | Role |
|---|---|---|
| 48 V input | 60 V eFuse (TPS2663 class); SMBJ58A; 100 µF / 100 V | One chip: electronic fuse, reverse polarity, inrush limit (hot plug), overvoltage cut |
| Strip supply | 100 V synchronous buck controller (LM5146 / LM5143 class) + 2 MOSFETs + 10–22 µH 10 A inductor + 100 V ceramics | 48 → 24 V or 12 V set by resistor or jumper per build. ~95 %, so ~10 W to shed at 200 W: copper and/or heatsink |
| Logic supply | 60 V small buck (LMR36006 class) → 5 V; LDO → 3.3 V | 5 V for the strip buffer, 3.3 V for the MCU |
| Receiver | ±60 V fault-protected RS-485 receiver, ≥ 10 Mb/s; 120 Ω; SM712 | Reads the pair, survives a miswired 48 V |
| MCU | **RP2040** + 2 MB QSPI flash + 12 MHz crystal (or RP2350 / RP2354: more RAM, flash in package on the 2354) | Checks the CRC, keeps the last good image, emits WS281x / APA102 cycle-exact from PIO |
| Strip output | 74AHCT2G125 at 5 V; series resistors; 5 V TVS | Clean DATA + CLOCK at the strip |
| Chain | ±60 V RS-485 driver + SM712 | Repeats to the next node |
| Local diagnostics | 2 LEDs: power; data (green = good frames, red = CRC errors or no signal) | Stands in for telemetry: on site you see whether the node receives |
| Update | USB-C (RP2040 UF2 boot ROM) + SWD header | Drag-and-drop firmware, no return channel needed |
| Connectors | In and out, 4 conductors keyed (48 V, GND, D+, D−), rated > 60 V and for the current; screw terminal to the strip | The most expensive line, and what makes miswiring impossible |

Design choices:

- **No galvanic isolation.** 48 V and data come down the same cable from the
  same PSU: the grounds are already common. An isolated receiver only helps a
  node with its own supply.
- **Addressed by position in the chain**, no DIP switches: a node consumes
  the frames for hop 0 and forwards the others with the hop count decremented.
- **Local current limit**: the MCU knows the pixels it emits; it caps
  brightness to the buck's rating, and dims when an NTC on the buck runs hot.
- **Over-the-link update without a return channel**: the controller
  broadcasts the image in a loop; each node checks the whole-image CRC before
  writing it. USB stays as the fallback.
- Rough parts cost, to confirm with a real BOM: **€20–40 per node** in small
  series, dominated by the 48 V connectors and the power stage.

---

## 6. Sizing — cables, bars per run, power supplies

All figures come from `tools/power_budget.py` (`--markdown` prints these
tables; `--bars --px --w-px --home …` checks one run of yours).

### Assumptions

| Item | Value | Why |
|---|---|---|
| Cable | **1.5 mm²** flexible copper, 13.3 Ω/km per conductor (IEC 60228 class 5, 20 °C) → **26.6 mΩ per metre of cable** (loop) | `--hot` adds 16 % for a cable at 60 °C |
| PSU | 48 V model **trimmed to 50 V** | the top of the Mean Well trim range (50.4 V, §6 references); still SELV |
| Bar | 1 m; full white **0.24 W/px RGB, 0.32 W/px RGBW** (all four on) | WS2815 / WS2814 class datasheet figures; size for full white — a blackout to full white flash happens |
| Node | buck **95 %** + 0.5 W (MCU, link) | constant-power load: a lower input voltage draws more current |
| Run | PSU → home run → first node, then **1 m jumpers** node to node | the ladder is solved exactly, every segment's drop counted |
| Limits | ≤ **10 A** per run, ≤ **10 %** cable loss, last node ≥ **36 V** | 10 A: connector pins and margin under the ~16 A thermal rating of 1.5 mm²; 10 % loss = 10 % of the power heating the cable |

Power per bar and node input:

- **A** — 1 m · 60 px RGB (12 V class): 14.4 W → 15.7 W at the node input
- **B** — 1 m · 60 px RGBW (24 V class): 19.2 W → 20.7 W
- **C** — 1 m · 144 px RGB: 34.6 W → 36.9 W

### Max bars on one 1.5 mm² run, by home-run length

| Bar | Bar W | Node in W | 5 m | 10 m | 20 m | 30 m | 50 m |
|---|---:|---:|---:|---:|---:|---:|---:|
| A — 1 m · 60 px RGB, 12 V class | 14.4 | 15.7 | 29 | 28 | 20 | 15 | 10 |
| B — 1 m · 60 px RGBW, 24 V class | 19.2 | 20.7 | 22 | 21 | 16 | 12 | 7 |
| C — 1 m · 144 px RGB | 34.6 | 36.9 | 12 | 12 | 10 | 7 | 4 |

Up to ~10 m the **10 A** limit sets the count (about 470 W per run); beyond,
the **10 % loss** does. A run gets ~10–15 % shorter with a hot cable
(`--hot`).

### Max home-run length, by number of bars

| Bar | 1 bars | 2 bars | 4 bars | 8 bars | 16 bars |
|---|---:|---:|---:|---:|---:|
| A | 540 m | 270 m | 134 m | 65 m | 29 m |
| B | 408 m | 204 m | 101 m | 49 m | 21 m |
| C | 229 m | 114 m | 56 m | 26 m | — |

Few bars go very far: the 36 V floor of the node is reached only past several
hundred metres, where the loss criterion has long stopped the run. In
practice **the length is bounded by the loss you accept, not by the node.**

### Why 48 V — the same 8 × 19.2 W (154 W) at 20 m

| Bus | Current | Last bar sees | Cable loss |
|---|---:|---:|---:|
| 12 V direct | — | drop over 12 V: impossible | — |
| 24 V direct | 6.4 A | 20.0 V (-17%) | 16% |
| 48 V (50) + buck | 3.5 A | 47.8 V → buck → 24 V | 4% |

At 12 V the cable alone would need to drop more than the supply. At 24 V the
last bar is 17 % under its rating (colour shift). At 48 V the node still has
~48 V and its buck hands the bar exactly 24 V.

### The link is not the limit

At 8 Mb/s (UART, 10 bits a byte, 14 bytes of frame overhead), a 60 fps frame
leaves ~15 ms after the latch and margins:

| Bar | Link time per bar | Bars per link at 60 fps | Bars per run by power (20 m) |
|---|---:|---:|---:|
| A — 60 px RGB | 0.24 ms | 61 | 21 |
| B — 60 px RGBW | 0.32 ms | 47 | 17 |
| C — 144 px RGB | 0.56 ms | 26 | 10 |

Power ends a run well before data does: one output can feed **two or three
power runs** (the chain passes a power injection point; the pair goes on, the
48 V of each run comes from its own feed).

### Sizing the supplies

**One PSU feeds one segment, never more.** A run (one output's chain) takes
one PSU or several; with several, the **48 V is cut** between their segments
(GND and the data pair go on): two PSUs on one 48 V conductor would fight
over the load at their slightly different voltages.

**Reasonable size: 480 W at 48 V (10 A)** — the same 10 A as the 1.5 mm² run
and its connectors, so:

- the PSU's own current limit (~105 % of rating) protects the cable; a fuse
  stays as backup, not as the only protection;
- a bigger PSU buys nothing past ~20 m: the cable's 10 % loss stops the
  segment first (table below);
- 480 W / 48 V is a common rating, fanless and sealed (IP67) for stage use.

Load it to **80 %**: trimmed to 50 V a 480 W PSU gives ~9.6 A, so a segment
is designed for **≤ 384 W** (node inputs at full white + cable loss).

Bars per segment, PSU at 80 % and every limit of the assumptions applied:

| Bar | PSU | 5 m | 10 m | 20 m | 30 m |
|---|---|---:|---:|---:|---:|
| A — 60 px RGB | 320 W | 15 | 15 | 15 | 14 |
|  | **480 W** | **23** | **22** | **20** | **15** |
|  | 600 W | 28 | 27 | 20 | 15 |
| B — 60 px RGBW | 320 W | 12 | 11 | 11 | 11 |
|  | **480 W** | **17** | **17** | **16** | **12** |
|  | 600 W | 21 | 21 | 16 | 12 |
| C — 144 px RGB | 320 W | 6 | 6 | 6 | 6 |
|  | **480 W** | **10** | **9** | **9** | **7** |
|  | 600 W | 12 | 12 | 10 | 7 |

600 W only helps on short home runs; from 20 m the cable sets the count.
320 W fits short or light segments (cheaper, lighter, a smaller short-circuit
current).

Reference PSUs (prices seen 2026-10, single unit, excl. VAT):

| Model | Rating | Trim | Cooling, case | Warranty | Price | Use |
|---|---|---|---|---|---|---|
| **Mean Well UHP-500-48** | 501 W, 10.45 A | 45.6–50.4 V | fanless, slim open case (needs a box), PFC, 95 % | 3 years | ~$105 (TRC), ~€133 (RS) | **best price per watt** in the 480 W class, in a rack or a vented case |
| **Mean Well HLG-480H-48A** | 480 W, 10 A | 40.8–50.4 V | fanless, IP65 aluminium, −40…+90 °C, PFC | 7 years | ~$143 (DigiKey), ~€148 (TME) | exposed on the rig, touring |
| Mean Well LRS-350-48 | 350 W, 7.3 A | — | **fan**, open frame | 3 years | ~$36 (TRC) | cheapest; workshop or tests, not on stage |

Both 480 W-class models stop at **50.4 V**, hence the 50 V of the
assumptions.

Per segment, also:

- **a fuse** at the PSU output (10 A, slow) as backup to its current limit;
  the node's eFuse only protects the node;
- **inrush**: every node's bulk capacitor charges at power-on; the eFuse
  soft-start spreads it, check the PSU's capacitive-load rating all the same;
- a **local current limit** in the node (§5) caps each bar at a chosen power:
  segments can then be sized on that cap instead of full white.

Example — 24 bars B, three segments of 8, home runs of 20 m: each segment
draws 173 W (3.5 A) at full white, so three 320 W PSUs suffice; with
480 W PSUs, two segments of 12 bars (266 W each) do the same job.

---

## 7. The pixfrog link — frame format (draft)

Line: UART 8N1 at **8 Mb/s** (4 Mb/s fallback for very long hops). On the
16 MHz PARLIO bus a bit is 2 samples (4 at 4 Mb/s), against 20 for an
800 kHz NRZ bit: **8× fewer samples per byte** than today, so less PSRAM
traffic and less encode time. Both encodings share the same bus clock, so
classic and differential outputs mix freely.

```
offset  size  field
0       2     sync      0x50 0x46 ("PF")
2       1     hop       0 = this node, 0xFF = broadcast; decremented on repeat
3       1     type      1 = pixels, 2 = latch, 3 = firmware chunk
4       1     protocol  led::Protocol of the strip (pixels frames)
5       1     flags     reserved
6       2     seq       frame sequence (latch names the seq to show)
8       2     length    payload bytes
10      n     payload   wire-ready bytes: order, gamma, brightness done by the controller
10+n    4     crc32     over type .. payload (hop excluded, so a repeater
                        rewrites it and forwards cut-through)
```

- **Pixels** frames carry wire-ready bytes: the controller has already applied
  colour order, gamma, white balance and brightness, as it does today. The
  node only adds the strip timing named by `protocol`.
- **Latch** is broadcast once all pixel frames are out: every node shows the
  same `seq` together (ArtSync end to end).
- **Cut-through repeat**: a node rewrites `hop` and forwards byte by byte, so
  a hop costs microseconds, not a frame.
- Budget: 1024 RGB pixels = 3072 B ≈ 3.9 ms at 8 Mb/s. A chain carries about
  four 1024-pixel nodes at 60 fps.

---

## 8. Controller firmware

- A per-output **Link** setting: *Direct* or *Differential*. It drives the
  expander bit and picks the encoder: NRZ / SPI samples, or link frames.
- The strip protocol stays where it is (WS2815, APA102, …); in differential
  mode it goes into the frame header instead of shaping the samples.
- Web, TFT and console show the Link per output; nothing to configure on the
  node.
- The CLOCK bus line of a differential output is unused.

---

## 9. Plan

1. **Bi-mode shield**: 74HCT125 + RS-485 drivers + expander; the Link setting
   and the frame encoder in the firmware, testable on the bench with a
   receiver before any node exists.
2. **Strip node**: RP2040, framed link with CRC, latch, chain, local current
   limit, update over the link; 48 V eFuse + buck sized with §6.

There is no intermediate "dumb" node carrying NRZ as is: it would not carry
clocked strips on one pair and would show corrupted frames, the two things the
link is for.

## 10. Open questions

- The 48 V connector: M12 L-coded (§4) or another family rated > 60 V, keyed
  against every XLR; cable assemblies with a twisted pair inside.
- Cable: one 4-conductor cable with a twisted 120 Ω pair, or DMX cable for the
  data beside a power cable.
- 4 vs 8 Mb/s on the longest hop with the chosen transceiver.
- Exact transceiver and buck part numbers, against stock and price.
