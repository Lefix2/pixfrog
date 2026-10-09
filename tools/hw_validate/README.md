# hw_validate — hardware regression suite

Replayable end-to-end validation of every feature **on the real board**,
grown from the one-shot scripts used during development. Run after any
flash (OTA or USB) to prove the device still behaves.

```bash
cd tools/hw_validate
./run_all.py                # everything except OTA (~3 min)
./run_all.py --with-ota     # + OTA round-trip (flashes the inactive slot)
./run_all.py scenes auth    # subset
./run_all.py --junit out/   # + out/<validator>.xml (JUnit, for a CI runner)
PORT=/dev/ttyACM1 BOARD_IP=10.0.0.5 ./run_all.py
```

| Validator | Proves |
|---|---|
| `artnet` | ArtDmx → universe pool → pixel decode (counter + pixr) |
| `sacn` | E1.31 unicast → pool → decode (opt-in flag honoured) |
| `failsafe` | never-active rule, colour fill, recovery, blackout, hold |
| `scenes` | generators, multi-colour blobs, solid strobe ends, channel mask, network priority, scene list add/rename/move/delete + persistence, ArtTrigger, boot scene |
| `show` | grand master / blackout / strobe (console + ArtTrigger KeyMacro), scene zones and crossfade, the DMX control universe over Art-Net (master, blackout, scene band, local stop holds, release when the desk goes silent), fixture profile |
| `output` | refresh bounds 20..120 Hz and the rate actually held, pixels above the budget kept, dead-pixel gaps (merge, persistence, logical pixels untouched) |
| `identify_gamma` | identify blink, gamma/wb readback, backup/restore round-trip |
| `fseq` | upload to SD, playback position/duration, console seek, ArtTimeCode seek, FPP MultiSync start/sync/stop/hot-join (needs a microSD in the board) |
| `display` | backlight level + idle dim + dim delay: console ranges, NVS persistence, web round-trip |
| `auth` | open-by-default, 401s, flat brute-force delay, UART recovery |
| `webops` | `/api/status` fields, gzipped SPA + ETag/304, mDNS: unique name + `pixfrog.local` alias (log, status, and a unicast query through `powershell.exe` when present), coredump cycle |
| `network` | the addressing applied without a reboot: a static address set from the console answers HTTP at once, a DHCP lease comes and is advertised, the box comes back to its own addressing (needs a DHCP server on the bench LAN) |
| `sacn_multicast` *(opt-in)* | E1.31 on the 239.255.x.y groups → decode; a universe re-config moves the join (5 s refresh) and drops the old group; the control universe on its group. `MCAST_FROM` = a sender on the board's LAN (from WSL the packets leave Windows through `mcast_send.ps1`) |
| `multiboard` *(opt-in)* | two boxes: peers, CORS, one `pixfrog.local` holder; the preferred box takes the alias, the sibling takes it back when the holder vanishes (≤ ~60 s); both `hub_preferred` restored. `OTHER_IP` = the sibling |
| `ota` | upload → slot swap → confirmation after 30 s of rendering; then a second upload reset before confirming → bootloader rollback, record on console + `/api/status`, web acknowledge (needs `build/pixfrog.bin`, ~3 min) |

Conventions (see `pixfrog_uart.py`):
- **One serial session per validator** — opening the port resets the board,
  so a validator never closes/reopens mid-run.
- Every validator **restores the board defaults** it touched (opt-in flags
  back to off, gamma to linear, …).
- UDP sends are **repeated/spread** — single datagrams behind a NAT
  routinely vanish on a cold ARP entry; treat one-packet tests as flaky.
- If `/dev/ttyACM0` vanishes after a replug, re-attach it to the test host
  (USB pass-through to a VM/container may need re-attaching).

The *opt-in* validators are not in `run_all`'s default list (they need a
sender on the LAN or a second box): `./run_all.py sacn_multicast multiboard`
with their variables set.

Not covered here: wire-level timing/levels (Saleae workflows — see the
repo skills).

## Wire-level checks (Saleae)

The validators above prove the chain as far as the pixel buffer, by reading the
board back over UART. They cannot see the encode/DMA path: a fault there leaves
`submits`, `current_fps` and `dma_underruns` all healthy while the bus sits
silent — PARLIO loop mode raises no completion event, so nothing in the firmware
contradicts itself. That is exactly how #74 shipped.

`nrz_decode.py` closes the gap by decoding what actually left the GPIO:

```bash
# 1. stream a known payload so any capture moment sees the same pixels
./artnet_stream.py --universe 1 --hex ff0102030405060708090a0b --seconds 25 &

# 2. capture the data line in Logic 2 (50 MS/s, 3.3 V) and
#    Export Raw Data -> CSV

# 3. decode it back and assert
./nrz_decode.py digital.csv --channel 0 --protocol ws2815 --pixels 4 \
    --expect ff0102030405060708090a0b
```

With dead-pixel gaps configured (`ch 0 gaps 2:1`), pass the same list to
`--gaps`: `--pixels`/`--expect` stay the logical strip and the decoder expects
the dark LEDs at their physical positions.

Which Saleae column carries which bus bit is wiring, not configuration —
establish it with `cal 1` (walking-1 across the 16 bits) rather than assuming.
`cal 0` (1 kHz square on all 16 GPIOs) is the fastest go/no-go: on a healthy
board every probed pin shows ~977 Hz.
