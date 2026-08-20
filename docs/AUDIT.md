# Project audit — pixfrog

Audit of the codebase at commit `43bcb0f` (`main`, 2026-06-22), covering
firmware components, host test coverage, CI, and documentation consistency.

Method: full read of `main/`, `components/` (excluding generated and vendored
sources), the CI workflow and the repo docs; replay of the host-testable CI
jobs (clang-format, seven unit suites, emulator build + smoke test) on this
machine. The two IDF builds (oled + tft) were **not** replayed here — no
ESP-IDF toolchain in this environment — so nothing below rests on a firmware
build or on hardware behaviour.

Findings are ordered by severity. Each one names the file and line so it can be
checked directly.

---

## Verification status

| CI job | Replayed | Result |
|---|---|---|
| `format-check` (clang-format 18) | yes | clean, 0 diffs |
| `host-tests` (7 suites) | yes | **29 589 assertions, 0 failures** |
| `emulator` (SDL2 build + smoke) | yes | builds, `SMOKE OK`, **1 compiler warning** (§L4) |
| `build-oled` / `build-tft` (IDF v5.5) | no | no toolchain available here |

Per-suite: `led_protocols` 27 984 · `dmx_logic` 1 333 · `artnet_parser` 123 ·
`sacn_parser` 57 · `fseq_parser` 38 · `config_store` 32 · `fpp_sync_parser` 22.

---

## H1 — Out-of-bounds access on the universe LUT (unvalidated 16-bit index)

`dmx_manager` keys its universe routing table on the Art-Net Port-Address,
which is 15-bit, and sizes the table accordingly:

```
components/dmx_manager/src/dmx_manager.cpp:73
uint16_t g_universe_to_slot[32768]{};
```

Seven functions index it with a caller-supplied `uint16_t universe_number` and
none of them range-check it (lines 459, 494, 502, 513, 520, 543, 551). Art-Net
is safe — `parse_dmx` masks Net to 7 bits, so its universe is always ≤ 32767
(`components/artnet/include/artnet_parser.h:60`). Two other ingest paths are
not.

### H1a — sACN (network-reachable)

The E1.31 parser deliberately accepts the full spec range and rejects nothing
above 32767:

```
components/sacn/include/sacn_parser.h:92
const uint16_t universe = be16(buf + 113);
if (universe < 1 || universe > 63999) return false;
```

`handle_data` then passes that value straight through
(`components/sacn/src/sacn.cpp:121,122,128,133,139`). A universe in
32768…63999 reads up to 64 KiB past the end of `g_universe_to_slot`. The
garbage read back is used as a slot index without further validation, so it
propagates into writes:

- `note_universe_terminated` (`dmx_manager.cpp:484`) →
  `g_slot_to_channel[slot]` (48 bytes) → `g_last_activity_us[ch] = 1` on an
  8-element array with `ch` up to 255 — an out-of-bounds `.bss` write from a
  **single sACN packet carrying the Stream-Terminated option**.
- `write_universe_from_source` (`dmx_manager.cpp:543`) → `g_merge[slot]`
  (48 entries), `g_merge_staging + slot*1024` and `g_uni_back + slot*512` —
  up to 512 bytes written at an attacker-influenced PSRAM offset of up to
  ~33 MB.

Reachable by any host on the LAN once `sacn_enabled` is on (opt-in, default
off — that is what keeps this out of "critical"). No authentication exists or
is possible on this path.

### H1b — FSEQ sparse ranges (file-reachable)

`SparseRange::start_channel` is a 32-bit file field
(`components/fseq_player/src/fseq_format.h:43`), converted with no upper bound:

```
components/fseq_player/src/fseq_format.h:115
return static_cast<uint16_t>(universe_base + abs_ch / 512u);
```

A `start_channel` above ~16.7 M yields a universe above 32767 (and wraps the
`uint16_t`), which `inject_sparse_frame` hands to `dmx::inject_universe`
(`dmx_manager.cpp:551`) — same unchecked index. `.fseq` files arrive over
`POST /api/fseq/upload`, which is unauthenticated in the default configuration
(see H2).

### H1c — Table build overflows on high universe numbers

Every configuration surface bounds `universe_start` to 0…32767 (console
`control_console.cpp:394`, web `web_config.cpp:646,953`, UI
`menu.cpp:1218`), but the LUT build adds the channel's span on top without
re-checking:

```
components/dmx_manager/src/dmx_manager.cpp:115
g_universe_to_slot[cc.universe_start + u] = slot;
```

The only guard in that loop is `slot < kNumUniverses`, not the index. A channel
at universe 32767 spanning 8 universes (see M1) writes 7 entries past the end
of a 64 KiB `.bss` array. `POST /api/autopatch` with `base: 32767` reaches the
same place — `compute_auto_patch` wraps its output to 15 bits, but
`rebuild_universe_lut` then adds `u` unwrapped.

**Suggested fix (one place, covers all of it):** replace the raw indexing with
a single bounds-checked accessor —

```cpp
constexpr uint16_t kMaxUniverseNumber = 32767;
inline uint16_t slot_for_universe(uint16_t n) {
    return n > kMaxUniverseNumber ? UINT16_MAX : g_universe_to_slot[n];
}
```

— use it at all seven call sites, and clamp the write in
`rebuild_universe_lut`. `channel_for_universe` should also validate
`slot < g_slots_used` before dereferencing `g_slot_to_channel`. The sACN
receiver can additionally drop universes above 32767 at parse time, since the
device can never map them.

A `dmx_logic`-level unit test for "universe number out of mappable range
resolves to no slot" would pin this down; today the pure-logic suite does not
cover the LUT, because the LUT lives in the IDF-only translation unit.

---

## H2 — Mutating REST endpoints are cross-site forgeable

`web_config` gates every POST behind `require_auth`, but that gate is a no-op
until an admin password is set, and no password is set by default:

```
components/web_config/src/web_config.cpp:176
if (!config::web_password_set()) return true;
```

There is no `Origin` check, no CSRF token, and no `Content-Type` requirement on
any handler (grep for `Content-Type` in `web_config.cpp` returns only the
response-side calls). Every mutation therefore qualifies as a CORS *simple
request*: a browser will send it cross-origin with no preflight. The response
is unreadable to the attacker, but the side effect lands.

So any page a LAN user visits can, against a default-configured pixfrog:

- `POST /api/factory-reset` — wipe the whole show configuration
- `POST /api/global`, `POST /api/channel/{n}` — re-patch universes, kill output
- `POST /api/reboot` — drop the rig mid-show
- `POST /api/ota` — flash arbitrary firmware (see M3)

Discovery is not a barrier: mDNS advertises `pixfrog.local` with a
`product=pixfrog` TXT record whenever the web UI is on
(`web_config.cpp:1284-1297`), and the aggregated dashboard already enumerates
peers by IP.

The comment at `web_config.cpp:146-149` states the design intent — "writes stay
protected by Basic auth, so opening reads carries no extra mutation surface" —
which holds only once a password exists. The gap is the default.

**Suggested fix:** reject POSTs carrying an `Origin` header that does not match
the request `Host` (browsers always send `Origin` on cross-origin writes;
`curl` and `uartctl` never send it, so scripted use is unaffected). That is a
handful of lines in `require_auth` and closes the class regardless of whether a
password is set. Prompting for a password on first web login would be a
worthwhile second layer, but it is a product decision, not a fix.

---

## M1 — Universe pool sizing assumes ≤ 6 universes per channel; RGBW needs 8

```
components/dmx_manager/include/dmx_manager.h:19
constexpr size_t kNumUniverses = 48;   // 8 channels × 6 max
```

The "6 max" holds for 3-byte pixels: 1024 px × 3 = 3072 B = 6 universes. For
RGBW protocols `bytes_per_pixel` returns 4 (`led_protocols.h:64`), so a
full-length channel needs 1024 × 4 / 512 = **8** universes, and eight such
channels need 64 slots.

`rebuild_universe_lut` stops silently when the pool runs out:

```
components/dmx_manager/src/dmx_manager.cpp:114
for (size_t u = 0; u < universes_used && slot < kNumUniverses; ++u) {
```

There is no log line, no capacity flag, and no UI indication. The affected
channels simply receive nothing — the failure looks like a patching mistake on
the console side. The default channel layout (`universe_start = 1 + idx*6`,
`config_store.cpp:44`) also overlaps itself under RGBW.

`validate_capacity` / `clamp_pixel_counts` guard the *DMA time* budget, not the
*universe pool* budget, so neither catches this.

Options: raise `kNumUniverses` to 64 (+8 KiB PSRAM for the banks, +16 KiB for
merge staging — cheap on a 32 MB part), or surface the exhaustion the same way
capacity overrun is surfaced today. Either way the `8 × 6 max` comment needs
correcting. README's "up to 48 universes" is accurate as a pool size but reads
as a per-config guarantee.

---

## M2 — PARLIO backend reallocates frame buffers from `render_task`

AGENT.md states the invariant plainly: *"No allocation on the hot path.
Everything `render_task` / ISR touches is allocated at boot."* The default
output backend does not hold to it.

`render_frame` calls `ensure_frame_capacity(needed)` every frame, and any
change in the encoded frame size — i.e. any `pixel_count`, `protocol` or
`grouping` edit — tears the unit down and rebuilds it:

```
components/led_output/src/parlio_output.cpp:178-180
if (g_unit && samples == g_fb_samples) return true;
destroy_unit();
return create_unit(samples);
```

`destroy_unit()` frees all three PSRAM buffers *before* `create_unit()` tries to
allocate the new ones, so a failed `heap_caps_aligned_calloc` (fragmentation
after many resizes) leaves no unit at all: `render_frame` returns false and the
LEDs stay dark. It retries each frame, so it recovers if memory frees — but the
window is unbounded and nothing logs it beyond a single `FB alloc failed`.

Allocating `max_samples_per_frame` once at boot and varying only the
transmitted length would restore the invariant. That is 3 × ~2.6 MB of PSRAM
reserved permanently, which the 32 MB part can carry.

I checked the adjacent hazard and it is **not** a bug: `create_unit` resets
`g_written[]` (line 159), so the shrink-memset at line 277-279 cannot run past
a newly-allocated smaller buffer.

---

## M3 — OTA images are not authenticated

`handle_ota` streams the body into the inactive slot and relies on
`esp_ota_end` for validation (`web_config.cpp:1033`). That checks the image
magic, chip ID and checksum — it does **not** verify a signature. Secure Boot
and signed-OTA verification are not enabled anywhere in `sdkconfig.defaults`,
and flash encryption is likewise off.

On its own this is a defensible trade-off for a LAN device. Combined with H2 it
is remote code execution against a box that has never had a password set. The
A/B rollback gate in `app_main` (`main.cpp:341-347`) is correctly wired and
would revert a *crashing* image, but not a hostile one that boots fine.

Fixing H2 removes the drive-by vector; signed OTA would be the durable answer
if the threat model warrants it.

---

## M4 — Password hashing has no key stretching

```
components/config_store/src/config_store.cpp:253
mbedtls_sha256_update(&ctx, salt, 8);
mbedtls_sha256_update(&ctx, password, strlen(password));
```

One SHA-256 round over an 8-byte random salt. The salt defeats rainbow tables
and the comparison is constant-time (`config_store.cpp:290-294`), which is the
part that is easy to get wrong and was gotten right. But an attacker who reads
NVS — physical access, or a flash dump, neither protected by flash encryption —
can brute-force a typical password essentially instantly. The 500 ms delay on
`require_auth` failure only bounds *online* guessing.

PBKDF2 with a few thousand iterations costs one dependency-free mbedTLS call
and runs once per login, not per request. Worth doing if the password is
expected to protect anything beyond casual misuse.

Related and worth stating in the docs rather than fixing: Basic auth over plain
HTTP puts the password on the wire, base64-encoded, on every mutation.

---

## L1 — Documentation drift

| Where | Says | Actually |
|---|---|---|
| `AGENT.md` module map | no entry for `fseq_player`, `fpp_sync`, `web_config` | all three exist; `web_config` is documented lower in the file, the other two nowhere |
| `AGENT.md` hard rules | ci-local replays "five host suites" | seven (`tools/ci-local.sh:15`) |
| `.claude/skills/host-tests/SKILL.md` | "the three pure-host unit test suites", runs 3 | CI runs 7; the skill silently under-tests |
| `AGENT.md` module map, `ui` row | "SSD1306 driver, seesaw encoder, menu FSM" | ST7789V TFT is the default display; OLED is the CI overlay |

`docs/ARCHITECTURE.md` is the exception — it tracks the PARLIO switch
accurately throughout and needs nothing.

The `host-tests` skill is the one that matters: an agent following it runs
three suites and believes the component contract in AGENT.md ("a change in
`led_protocols`, `dmx_manager`, or `artnet` requires the matching host suite
green") is satisfied, while `config_store`, `sacn`, `fseq_player` and
`fpp_sync` go unrun.

## L2 — `main.cpp` still describes the legacy backend

The boot-sequence header says `3. LCD_CAM driver` (`main.cpp:6`), the render
loop comments refer to "the LCD_CAM encoder" and "the LCD_CAM render"
(lines 198, 201), the InitConfig is named `lcd_cfg` and the failure path logs
`"lcd_cam init failed — aborting"` (line 301) — all on the path that now runs
PARLIO by default. `led_protocols.h:98` likewise still sizes for "2 FBs"
(~5.2 MB) where PARLIO uses three.

## L3 — Comments referencing a retired numbering scheme

18 comments across `main/main.cpp`, `components/ui/include/ui.h`,
`components/artnet/include/artnet.h` and
`components/led_output/src/lcd_cam_output.cpp` refer to "Item 3", "Item 7",
"Item A5", "TODO B2", "TODO B5". None of those identifiers exist in `TODO.md`,
which is now organised by section. They read as live cross-references and
resolve to nothing.

Also: the doc comment describing `nvs_load_blob` sits above
`fill_default_scenes` (`config_store.cpp:59-64`) — a function moved between the
comment and its subject at some point.

## L4 — Compiler warning in the default build

```
components/ui/src/menu.cpp:40: warning: 'draw_row' defined but not used [-Wunused-function]
```

`draw_row` is defined unconditionally but every call site sits in an
`#else`/OLED branch, so it is dead in TFT builds — which is both the default
firmware and the CI emulator job. Guarding the definition with
`#ifndef CONFIG_PIXFROG_DISPLAY_TFT` clears it. It is the only warning in the
emulator build.

## L5 — Smaller observations

- **Only `render_task` is watchdog-registered** (`main.cpp:163`). A wedged
  `artnet_rx`, `sacn_rx` or `ui` task produces no panic and no telemetry —
  output simply freezes on the last frame while FPS keeps counting.
- **`g_ota_in_progress` / `g_fseq_upload_in_progress` are plain `bool`**
  (`web_config.cpp:986`, `:1183`) used as mutual-exclusion guards. Correct
  under the default single-threaded httpd, fragile if `max_open_sockets` or
  the handler task count ever changes.
- **Test output convention diverges**: `fseq_parser` prints
  `38 passed, 0 failed`; the other six print `PASS=<n> FAIL=0`. Any future
  "did the suite pass" scraping has to special-case it.

---

## What is in good shape

Worth recording, because it is what makes the findings above cheap to fix:

- **The protocol layer is genuinely host-testable.** Splitting pure logic into
  `dmx_logic.h`, `artnet_parser.h`, `sacn_parser.h` and `fseq_format.h` — headers
  with no IDF dependency — is why there are 29 589 assertions instead of a
  hardware-only test plan. The 1 333 `dmx_logic` cases cover sizing, capacity
  and merge arbitration thoroughly.
- **No unsafe C string APIs in first-party code.** `strcpy`, `sprintf`,
  `strcat` and `alloca` appear nowhere outside vendored `third_party/`. Every
  buffer fill goes through a bounded helper.
- **The concurrency model is stated and followed.** Atomic pointer swaps rather
  than mutexes, ISRs limited to a semaphore give, no NVS writes off `ui_task`.
  The rules in AGENT.md match what the code does — H1 and M2 are the only
  places where the code drifts from its own stated invariants.
- **The output backends explain themselves well.** The header comments in
  `parlio_output.cpp` (loop-mode semantics, why three buffers, why the
  64-sample quantum) are the kind that answer the question a reader will
  actually have. The `power_vdd_io5_pads` comment in `main.cpp` — recording a
  measured 1.2 V on GPIO47 and why — is a model of a "why" comment.
- **Opt-in network surfaces are real.** sACN, FPP MultiSync and the web server
  each open no socket while their flag is off; verified by reading the start
  paths, not just the docs.
- **CI is proportionate**: docs-only branches skip the heavy jobs through a
  fail-safe allowlist, and `tools/ci-local.sh` genuinely replays the same jobs.

---

## Suggested order of work

1. **H1** — bounded accessor in `dmx_manager` plus the `rebuild_universe_lut`
   clamp. Small, self-contained, and closes a remote memory-corruption path.
   Add the `dmx_logic` test alongside it.
2. **H2** — `Origin` check in `require_auth`. Also small, and it is what makes
   M3 tolerable.
3. **L1** — fix the `host-tests` skill to run all seven suites, and the AGENT.md
   module map. Cheap, and it stops the next agent from under-testing.
4. **M1** — decide between raising `kNumUniverses` and surfacing exhaustion.
5. **M2** — boot-time allocation in the PARLIO backend.
6. **M4**, **L2**, **L3**, **L4**, **L5** — hygiene, batchable.

None of 1–3 touches hardware behaviour, so all three are provable with the
existing host suites plus `tools/ci-local.sh`.
