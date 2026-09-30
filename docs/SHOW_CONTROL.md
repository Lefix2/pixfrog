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

## Scene zones and crossfades

Each output plays its own scene or the live input. Starting a scene claims the
outputs of its channel mask (limited to a requested group, if one is given) and
leaves the others alone. Two scenes with masks 1–4 and 5–8 therefore run side by
side, and a scene started on an overlapping group takes those outputs over.
`scene stop <n>` stops one scene; `scene stop` stops them all.

Every change of source crossfades over the scene fade time (`scene_fade_ms`,
0–25.5 s, eased). This covers live → scene, scene → scene and scene → live.
The fade is global, and the desk's Fade channel overrides it.

## DMX control universe

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
| Speed / Param | 0 = the scene's own, 1–255 = override |
| Effect | 0 = the scene's own, 1–255 spread over the 11 effects |
| Red / Green / Blue (colour n) | overrides colour n of the playing scene; all three at 0 = the scene's own |
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
| Full (16 ch) | master 16-bit, blackout, strobe, scene, speed, param, effect, colour 1 RGB, colour 2 RGB, fade, FSEQ |

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
