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
- The 48 V is injected at a power point, **never on the shield's XLR**.
- This is the architecture of Martin's first-generation VDO Sceptron, sold
  for years (§11).

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
| Miswiring | **keyed connectors**: 48 V can never land on a data pin, nor on a 12/24 V strip; the shield's XLR never carries it |

The 5 V TVS of the current shield would also clip the RS-485 common-mode
range: the differential path gets RS-485-rated TVS instead.

### The 48 V connector

Two options, both carrying 48 V, GND, D+, D− on four poles.

**XLR4 — possible, under conditions.** Martin's first VDO Sceptron runs
48 V DC and its data pair on 4-pin XLR at the PowerPort outputs (§11), so it
is done in the trade. The limits it must respect:

- **48 V, not trimmed up.** A Neutrik XLR is rated **< 50 V** and 10 A per
  contact (NC4FX): the PSU stays at 48 V (§6 assumes it).
- **8 A per run**, as Martin fuses it (7.5–8 A), not 10 A: margin on a 10 A
  contact.
- **Never (un)plugged live.** Martin's label says so too: a DC arc at 48 V
  and several amps does not die out by itself, the contacts pit and heat. Pin
  1 (GND) mating first only helps on plug-in.
- **The risk of this rig, not of the connector**: today's XLR4 carries 12/24 V
  strip supply (`HARDWARE.md` §8, pin 4 = VCC). A 48 V cable into an old
  satellite or a direct strip destroys it at once. XLR4 for 48 V needs the
  whole park moved to 48 V, or a different connector on the bar side (Martin
  uses its own BBD there and XLR4 only at the PowerPort), or unmistakable
  marking.
- Thin XLR4 leads (audio, DMX, camera: 0.25–0.5 mm²) must never be used on a
  run.

**M12 L-coded — rated for it.** Five poles, DC up to 63 V and 16 A per pin,
IP67, screw lock, up to 2.5 mm²: 48 V, GND, D+, D−, the fifth pole for the
shield. It mates with nothing else on the rig, and the screw lock makes an
unplug under load deliberate. Dearer and slower to plug than an XLR.

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
| PSU | **48 V**, not trimmed up | an XLR is rated < 50 V (§4); SELV either way |
| Bar | 1 m; full white **0.24 W/px RGB, 0.32 W/px RGBW** (all four on) | WS2815 / WS2814 class datasheet figures; size for full white — a blackout to full white flash happens |
| Node | buck **95 %** + 0.5 W (MCU, link) | constant-power load: a lower input voltage draws more current |
| Run | PSU → home run → first node, then **1 m jumpers** node to node | the ladder is solved exactly, every segment's drop counted |
| Limits | ≤ **8 A** per run, ≤ **10 %** cable loss, last node ≥ **36 V** | 8 A: Martin's fuse on the same architecture (§11), margin on a 10 A contact and under the ~16 A thermal rating of 1.5 mm²; 10 % loss = 10 % of the power heating the cable |

Power per bar and node input:

- **A** — 1 m · 60 px RGB (12 V class): 14.4 W → 15.7 W at the node input
- **B** — 1 m · 60 px RGBW (24 V class): 19.2 W → 20.7 W
- **C** — 1 m · 144 px RGB: 34.6 W → 36.9 W

### Max bars on one 1.5 mm² run, by home-run length

| Bar | Bar W | Node in W | 5 m | 10 m | 20 m | 30 m | 50 m |
|---|---:|---:|---:|---:|---:|---:|---:|
| A — 1 m · 60 px RGB, 12 V class | 14.4 | 15.7 | 23 | 22 | 19 | 14 | 9 |
| B — 1 m · 60 px RGBW, 24 V class | 19.2 | 20.7 | 17 | 17 | 15 | 11 | 7 |
| C — 1 m · 144 px RGB | 34.6 | 36.9 | 10 | 9 | 9 | 6 | 4 |

Up to ~10 m the **8 A** limit sets the count (about 380 W per run); beyond,
the **10 % loss** does. A run gets ~10–15 % shorter with a hot cable
(`--hot`).

### Max home-run length, by number of bars

| Bar | 1 bars | 2 bars | 4 bars | 8 bars | 16 bars |
|---|---:|---:|---:|---:|---:|
| A | 498 m | 249 m | 124 m | 60 m | 26 m |
| B | 376 m | 188 m | 93 m | 45 m | 19 m |
| C | 211 m | 105 m | 52 m | 24 m | — |

Few bars go very far: the 36 V floor of the node is reached only past several
hundred metres, where the loss criterion has long stopped the run. In
practice **the length is bounded by the loss you accept, not by the node.**

### Why 48 V — the same 8 × 19.2 W (154 W) at 20 m

| Bus | Current | Last bar sees | Cable loss |
|---|---:|---:|---:|
| 12 V direct | — | drop over 12 V: impossible | — |
| 24 V direct | 6.4 A | 20.0 V (-17%) | 16% |
| 48 V + buck | 3.6 A | 45.7 V → buck → 24 V | 4% |

At 12 V the cable alone would need to drop more than the supply. At 24 V the
last bar is 17 % under its rating (colour shift). At 48 V the node still has
~46 V and its buck hands the bar exactly 24 V.

### The link is not the limit

At 8 Mb/s (UART, 10 bits a byte, 14 bytes of frame overhead), a 60 fps frame
leaves ~15 ms after the latch and margins:

| Bar | Link time per bar | Bars per link at 60 fps | Bars per run by power (20 m) |
|---|---:|---:|---:|
| A — 60 px RGB | 0.24 ms | 61 | 19 |
| B — 60 px RGBW | 0.32 ms | 47 | 15 |
| C — 144 px RGB | 0.56 ms | 26 | 9 |

Power ends a run well before data does: one output can feed **two or three
power runs** (the chain passes a power injection point; the pair goes on, the
48 V of each run comes from its own feed).

### Sizing the supplies

**One PSU feeds one segment, never more.** A run (one output's chain) takes
one PSU or several; with several, the **48 V is cut** between their segments
(GND and the data pair go on): two PSUs on one 48 V conductor would fight
over the load at their slightly different voltages.

**Reasonable size: 480 W at 48 V (10 A), behind an 8 A fuse.** Loaded to
80 % it delivers 8 A — exactly the run's limit — so:

- the PSU runs cool at the run's full load, and a bigger one buys nothing:
  the 8 A of the run, then the cable's loss past ~20 m, stop the segment
  first (table below);
- the **8 A fuse** protects the cable: the PSU's own current limit (~10.5 A)
  sits above the run's rating;
- 480 W / 48 V is a common rating, fanless and sealed (IP65/67) for stage use.

A 320 W PSU (6.7 A) is the other sensible size: its own current limit stays
under 8 A, so it protects the run by itself; it suits short or light segments
(cheaper, lighter, a smaller short-circuit current).

Bars per segment, PSU at 80 % and every limit of the assumptions applied:

| Bar | PSU | 5 m | 10 m | 20 m | 30 m |
|---|---|---:|---:|---:|---:|
| A — 60 px RGB | 320 W | 15 | 15 | 15 | 14 |
|  | **480 W** | **23** | **22** | **19** | **14** |
| B — 60 px RGBW | 320 W | 12 | 11 | 11 | 11 |
|  | **480 W** | **17** | **17** | **15** | **11** |
| C — 144 px RGB | 320 W | 6 | 6 | 6 | 6 |
|  | **480 W** | **10** | **9** | **9** | **6** |

A 600 W PSU gives exactly the 480 W figures: the run, not the PSU, is the
limit.

Reference PSUs (prices seen 2026-10, single unit, excl. VAT):

| Model | Rating | Trim | Cooling, case | Warranty | Price | Use |
|---|---|---|---|---|---|---|
| **Mean Well UHP-500-48** | 501 W, 10.45 A | 45.6–50.4 V | fanless, slim open case (needs a box), PFC, 95 % | 3 years | ~$105 (TRC), ~€133 (RS) | **best price per watt** in the 480 W class, in a rack or a vented case |
| **Mean Well HLG-480H-48A** | 480 W, 10 A | 40.8–50.4 V | fanless, IP65 aluminium, −40…+90 °C, PFC | 7 years | ~$143 (DigiKey), ~€148 (TME) | exposed on the rig, touring |
| Mean Well LRS-350-48 | 350 W, 7.3 A | — | **fan**, open frame | 3 years | ~$36 (TRC) | cheapest; workshop or tests, not on stage |

Both 480 W-class models trim up to 50.4 V; leave them at 48 V for the XLR
rating. Martin names the Mean Well SP-480-48 as a generic 48 V supply for
the first Sceptron.

Per segment, also:

- **an 8 A fuse** at the PSU output (slow-blow) whenever the PSU can source
  more than 8 A; the node's eFuse only protects the node;
- **inrush**: every node's bulk capacitor charges at power-on; the eFuse
  soft-start spreads it, check the PSU's capacitive-load rating all the same;
- a **local current limit** in the node (§5) caps each bar at a chosen power:
  segments can then be sized on that cap instead of full white.

Example — 24 bars B, three segments of 8, home runs of 20 m: each segment
draws 173 W (3.6 A) at full white, so three 320 W PSUs suffice; with
480 W PSUs, two segments of 12 bars (268 W, 5.6 A each) do the same job.

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
5       1     flags     bit 0: 16-bit components (a future 16-bit LED driver); rest reserved
6       2     seq       frame sequence (latch names the seq to show)
8       2     length    payload bytes
10      n     payload   wire-ready bytes: order, gamma, brightness done by the controller
10+n    4     crc32     over type .. payload (hop excluded, so a repeater
                        rewrites it and forwards cut-through)
```

- **Pixels** frames carry wire-ready bytes: the controller has already applied
  colour order, gamma, white balance and brightness, as it does today. The
  node only adds the strip timing named by `protocol`.
- **16 bits** are foreseen in the header, not used by WS281x strips (8 bits):
  Martin's bars process 16 bits per colour (§11); the link stays ready for a
  16-bit LED driver in the node.
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

- The 48 V connector: XLR4 (48 V, 8 A, the whole park moved to 48 V or a
  distinct bar-side connector) or M12 L-coded (§4); cable assemblies with a
  twisted pair inside.
- Cable: one 4-conductor cable with a twisted 120 Ω pair, or DMX cable for the
  data beside a power cable.
- 4 vs 8 Mb/s on the longest hop with the chosen transceiver.
- Exact transceiver and buck part numbers, against stock and price.

---

## 11. Prior art — Martin VDO Sceptron

Two generations of Martin's pixel batten frame this study.

### First generation (VDO Sceptron 10 / 20 / 40) — this architecture

| | VDO Sceptron | This study |
|---|---|---|
| Hybrid cable | **4-pin XLR** at the PowerPort, custom 6-pin BBD (IP66) at the bar | XLR4 or M12 L (§4) |
| Carries | **48 V DC + one data pair** | 48 V, GND, D+, D− |
| On the pair | **P3** (video, 16-bit) or **DMX512** (8-bit); a lead-out cable gives standard DMX on XLR5 at the end of a chain | the pixfrog link |
| Converter | P3 PowerPort 1500 / 1000 IP: P3 over Ethernet in, the pair out | pixfrog |
| Limits | **50 m** per chain (bars + cable), **7.5–8 A** fuse on a generic PSU, **63 bars** per output, 4 outputs per PowerPort | 8 A, 10 % loss, 36 V |
| Termination | built into the bars | 120 Ω in the node |
| Generic PSU | Mean Well SP-480-48 | UHP-500-48 / HLG-480H-48A |
| Live plugging | label: "Do not connect or disconnect live power cables" | same rule |

### Second generation (VDO Sceptron XB) — Ethernet in the bar

| | VDO Sceptron XB |
|---|---|
| Power | **48 V DC ±4 %**; 30 W/m full white, 16.9 W/m half power, 3.3 W/m idle |
| Cable | **DC + Ethernet**, custom 7-pin DCE, IP65, "all connections hot plugging compatible" |
| Data | **10/100 Ethernet in every bar**: Art-Net, sACN, RDM over Art-Net, P3, detected automatically; firmware over Ethernet |
| Data reach | 70 m between devices, a booster beyond |
| Supplies | PowerPort 2000 (4 outputs), 500 IP (1), DCE PSU 240 IP — a passive injector: Ethernet goes through it as is — or a generic 48 V PSU of 5 A, with a **10 A fuse** if it does not limit current |
| Power cap | full / half power switch on the supply, doubling the length of bars per run |
| Image | 16 bits per colour, LED PWM 1 or 4 kHz, per-pixel calibration |
| Protection | thermal sensor, thermal throttling, output limited above 50 °C |
| On the bar | multi-colour status LED, magnetic sensor for tests and reset, 20-scene stand-alone mode |
| Size | 27 × 37 mm profile, 1.6 kg/m, LEDs in resin, convection cooled |

Its chain table (metres of bars, 30 W/m, full power):

| Total chain | 25 m | 50 m | 75 m | 100 m | 150 m | 200 m |
|---|---:|---:|---:|---:|---:|---:|
| Bars | 12 m | 11 m | 9 m | 7 m | 4 m | 3 m |

`power_budget.py` reproduces it exactly with a ~2 mm² conductor (AWG14 is
2.08 mm²), a **37 V** floor at the bar and an **8 A** cap per output — an
inference, not a published figure: the bar regulates internally down to
~37 V, as the node does here down to 36 V.

### What it tells this study

- **48 V regulated in the bar is validated** twice, and a 27 mm profile holds
  the electronics: the 230 V-per-bar option loses its case.
- **8 A per run** is the manufacturer's figure: this study uses it (§6).
- **A half-power mode** as well as the node's current limit: Martin's way to
  double the length of a run.
- **16 bits in the frame** (§7), for a later LED driver.
- **Hot plugging** costs a dedicated connector (staggered pins, soft start);
  without it, the rule stays "PSU off".
- **The pair or Ethernet.** Gen 1 needed a PowerPort to turn P3 into the pair;
  the XB puts Ethernet in each bar, so any Art-Net / sACN desk drives it
  directly and the supply only injects power — at the cost of a 2-port switch,
  PHYs and magnetics per bar, ~1–3 W idle, 70 m reach and per-bar
  addressing. Here pixfrog is the controller and the nodes stay
  configuration-free: the pair (gen 1) fits that. Ethernet in the bar is the
  route if the bars must one day work without a pixfrog.

Sources: [VDO Sceptron family user manual, rev. E](https://adn.harmanpro.com/site_elements/executables/7220_1526701101/35000284e_UM_VDO-Sceptron_EN_E_original.pdf);
[VDO Sceptron XB spec sheet](https://adn.harmanpro.com/site_elements/resources/26178_1767652336/Martin_VDO_SceptronXB_SpecSheet_Rev3_01_26_original.pdf);
[VDO Sceptron XB user manual (FR), rev. B](https://adn.harmanpro.com/site_elements/resources/25700_1744142321/UM-SFTY_VDO-Sceptron-XB_FR_B_original.pdf).
