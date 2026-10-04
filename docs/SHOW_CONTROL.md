# Show control

Grand master, blackout and strobe, scene zones and crossfades, and the DMX
control universe that drives them from any lighting desk.

## Grand master, blackout, strobe

Three runtime controls sit after everything an output renders (network, scene,
FSEQ, failsafe). Each can target all outputs or a group:

| Control | Effect |
|---|---|
| Master | scales the output, 0–100 % (16-bit from the desk) |
| Blackout | output dark |
| Strobe | short flashes (≤ 30 ms) at 1–25 Hz |

They reset at reboot (full, lit, steady). Local values (web, TFT, UART,
ArtTrigger) and the desk's combine: the masters multiply, the blackouts add up,
and the faster strobe wins. Identify and the pixel-count ruler ignore all three,
so a strip can still be found during a blackout.

| From | How |
|---|---|
| Web UI | the SHOW card on the dashboard |
| TFT | OUTPUT → Master / Blackout / Strobe / Fade |
| UART | `show master <0..100> [outputs]`, `show blackout on\|off\|toggle [outputs]`, `show strobe <0..25> [outputs]`, `show fade <ms>` |
| REST | `POST /api/show {"outputs":15,"master":80,"blackout":true\|false\|"toggle","strobe_hz":5}` |
| ArtTrigger | Oem 0xFFFF, Key 1 (KeyMacro): SubKey 1 = toggle, 2 = on, 3 = off |

`outputs` is a bitmask: bit 0 = output 1, so `0f` = outputs 1–4.

## Effects and scenes

An **effect** is a look, kept in a bank of up to 31: a generator, one to four
colours, a speed and a parameter. It has no target. A **scene** is a memory of
up to eight **parts**, each sending one effect of the bank to a set of outputs
("effect 5 on outputs 1–4, effect 1 on outputs 5–8") with a fixture mode
(PROTOCOLS §5.5). An output belongs to one part at most; a scene leaves the
outputs of no part alone. Editing an effect changes every scene that plays it.

The eleven generators, each over six seconds on a 144-pixel line (time going
right, pixel 0 at the top), rendered by the firmware's own `fill_effect_run`
(`tools/effects_gallery/gallery.py`):

![The eleven generators as space-time strips](img/effects/effects-gallery.png)

Speed runs 0–255, in steps of two generator units: 2 px/s per step for chase,
scanner and stripes (up to 510 px/s), 20 °/s for rainbow. Solid is apart: its
speed is a strobe frequency, 0–60 Hz, where 255 means "steady colour 2".

### Dimmer phaser and invert

An effect can carry a **dimmer phaser**: a waveform that dims what the
generator draws, travelling along the pixels of the run (a fixture, or the
chained fixtures, as the part's fixture mode says).

| Setting | Meaning |
|---|---|
| Wave | none, sine, cosine, ramp up, ramp down, triangle, PWM, bump (a half-sine hump) |
| Rate | 0–255, 0.05 Hz (3 BPM) a step — 0 is a still wave, 255 is 12.75 Hz |
| Spread | phase shift along the run, 1/16 cycle a step: 0 = every pixel in phase (the run breathes as one), 16 = one whole wave along the run |
| Width | the share of the cycle the wave takes (n/255, 0 = all of it): it runs compressed, then holds its end level. For PWM, the lit share (0 = half) |
| Floor | the dimmer's low point (n/255): the wave runs between the floor and full |
| Reverse | the wave travels towards the start of the run |

**Dimmer invert** is the intensity negative of the whole effect, with or
without a phaser: a pixel as bright as colour 1 goes dark, a dark pixel takes
colour 1, the hue is kept in between. A chase becomes a lit strip with a dark
gap running through it; a sine phaser shifts by half a cycle; a steady colour
alone goes dark.

UART: `fx phaser <n> <wave> [<rate> <spread> [<width> <low> [reverse|forward]]]`,
`fx invert <n> 0|1`. REST: `"phaser": {"wave", "rate", "spread", "width", "low",
"reverse"}` and `"invert"` in the effect.

### Block, Groups, Wings

A desk's MAtricks, applied to the **pixels** of the run an effect is drawn on
(a fixture, the chained fixtures or the whole strip, as the part's fixture mode
says). The generator and the phaser are drawn on a shorter virtual run, which
is then spread over the pixels:

| Setting | Effect on a 24-pixel run |
|---|---|
| Block *N* | *N* neighbouring pixels share one value — block 3: 8 values, each 3 pixels wide |
| Groups *N* | the pattern repeats every *N* values — groups 6: the same 6 pixels four times |
| Wings *N* | the run splits in *N* parts, every other one mirrored — wings 2: the effect runs in from both ends |

0 or 1 turns a setting off; the three combine (wings first, then blocks, then
the repeat). UART: `fx matricks <n> <block> <groups> <wings>`. REST:
`"matricks": {"block", "groups", "wings"}` in the effect.

| Edit | Effects | Scenes |
|---|---|---|
| Web UI | **Effects** screen | **Scenes** screen: the parts, their outputs, effect and fixture mode |
| UART | `fx`, `fx add [name]`, `fx set <n> <generator> <rrggbb[,…]> <speed> <param>`, `fx name`, `fx move`, `fx del` (refused while a scene plays it) | `scene`, `scene add [name]`, `scene part <n> <outputs-hex> <effect> [each\|strip\|chain\|mirror]`, `scene clear <n>`, `scene name`, `scene move`, `scene del` |
| REST | `POST /api/effect/<n>`, `/api/effects/add\|move` | `POST /api/scene/<n>`, `/api/scenes/add\|move` |

Scenes stored by an older firmware are converted at the first boot: each
becomes one effect and one single-part scene, at the same position and the
same pace. A backup taken before the effect bank restores the same way.

## Scene zones and crossfades

Each output plays its own scene or the live input. Starting a scene claims the
outputs of its parts (limited to a requested group, if one is given) and
leaves the others alone. Two scenes on outputs 1–4 and 5–8 therefore run side by
side, and a scene started on an overlapping group takes those outputs over.
`scene stop <n>` stops one scene; `scene stop` stops them all.

| From | Play scene *n* on a group |
|---|---|
| Web UI | scene editor → ▶ Play: the scene on the outputs of its parts; ■ Stop stops that scene only. The ▶ of the scene list does the same. |
| UART | `scene play <n> <outputs-hex>` (e.g. `scene play 0 01`, `scene play 1 02`), `scene stop <n>` |
| REST | `POST /api/scene/<n>/play {"outputs":1}`, `POST /api/scene/<n>/stop` |
| Desk | two Scene slots with different output groups in the control universe |
| TFT, ArtTrigger | play on the outputs of the scene's parts — set the parts first |

Every change of source crossfades over the scene fade time (`scene_fade_ms`,
0–25.5 s, eased). This covers live → scene, scene → scene and scene → live.
The fade is global, and the desk's Fade channel overrides it.

## DMX control universe

![The DMX control editor in the web UI: preset, slots and their outputs](img/web-control.png)

A fixture mode you compose yourself. It starts at an address in a universe
(Art-Net port-address, or the same sACN universe), and each slot below takes
one DMX channel (two for a 16-bit master) in list order. Every slot acts on
an output group.

| Function | Value |
|---|---|
| Master | intensity; 16-bit with the fine channel next |
| Blackout | ≥ 128 = dark |
| Strobe | 0 = off, 1–255 = 1–25 Hz |
| Scene | bands of 8: 0–7 = none, 8–15 = scene 1, 16–23 = scene 2 … |
| Effect (`bank`) | bands of 8: 0–7 = the effect the scene's part plays, 8–15 = effect 1 of the bank, 16–23 = effect 2 … — played on the slot's outputs in place of the scene's own, for as long as the fader stays there |
| Speed / Param | 0 = the effect's own, 1–255 = override |
| Generator (`effect`) | 0 = the effect's own generator, 1–255 spread over the 11 generators |
| Red / Green / Blue (colour n) | overrides colour n of the effect an output plays; all three at 0 = the effect's own |
| Phaser wave | bands of 8: 0–7 = the effect's own, 8–15 = no phaser, 16–23 = sine, then cosine, ramp up, ramp down, triangle, PWM, bump |
| Phaser rate / spread / width | 0 = the effect's own, 1–255 = override |
| Block / Groups / Wings | 0 = the effect's own, 1 = off, 2–255 = N |
| Fade | scene fade time, value × 0.1 s |
| FSEQ | bands of 8: 0–7 = stop, 8–15 = file 1 … (the order of the file list) |
| Spare | nothing; it keeps a channel free |

**How the desk and local actions interact:**
- **Levels follow the desk continuously:** master, blackout, strobe, overrides
  and fade.
- **Triggers act when their band changes:** scene and FSEQ.
  - A scene started from the web keeps playing while the desk's Scene fader
    stays put.
  - When the universe first appears, only non-zero bands act, so a desk at 0
    does not stop a local scene. A desk already sitting on a scene starts it.
- **Zones from the desk:** use two Scene slots with different groups (for
  example outputs 1–4 and 5–8).
- **When the desk goes silent (3 s):**
  - master, blackout and strobe return to neutral, so the rig is never left
    dark;
  - the fade time returns to the configured one;
  - scenes and overrides hold.
- **Switching the control universe off** also clears the overrides.

**Presets:**

| Preset | Channels |
|---|---|
| Simple (6 ch) | master 16-bit, blackout, strobe, scene, fade |
| Full (16 ch) | master 16-bit, blackout, strobe, scene, speed, param, generator, colour 1 RGB, colour 2 RGB, fade, FSEQ |

**Editors:**
- web: *DMX control*, with the channel map and overlap and overflow warnings;
- TFT: INPUTS → DMX ctrl;
- UART: `ctrl`, `ctrl preset simple|full`, `ctrl add <fn> [mask] [colour] [fine]`, `ctrl set|del`, `ctrl universe|address|enable`;
- REST: `POST /api/control`.

The control universe may share a universe with an output, for example at
address 400 of the universe that feeds output 1. Both then read the same data.

### Fixture profile

`GET /api/control/fixture` (the *Fixture profile* button) downloads the current
mode as an [Open Fixture Library](https://open-fixture-library.org) JSON
profile, with named channels, scene names on the Scene bands, effect ranges and
strobe speeds. Import it into QLC+, or convert it on open-fixture-library.org
for other desks, then patch one fixture at the configured address.
