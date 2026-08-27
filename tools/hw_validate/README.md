# hw_validate — hardware regression suite

Replayable end-to-end validation of every feature **on the real board**,
grown from the one-shot scripts used during development. Run after any
flash (OTA or USB) to prove the device still behaves.

```bash
cd tools/hw_validate
./run_all.py                # everything except OTA (~3 min)
./run_all.py --with-ota     # + OTA round-trip (flashes the inactive slot)
./run_all.py scenes auth    # subset
PORT=/dev/ttyACM1 BOARD_IP=10.0.0.5 ./run_all.py
```

| Validator | Proves |
|---|---|
| `artnet` | ArtDmx → universe pool → pixel decode (counter + pixr) |
| `sacn` | E1.31 unicast → pool → decode (opt-in flag honoured) |
| `failsafe` | never-active rule, colour fill, recovery, blackout, hold |
| `scenes` | generators, channel mask, network priority, ArtTrigger, boot scene |
| `identify_gamma` | identify blink, gamma/wb readback, backup/restore round-trip |
| `display` | backlight level + idle dim + dim delay: console ranges, NVS persistence, web round-trip |
| `auth` | open-by-default, 401s, flat brute-force delay, UART recovery |
| `ota` | upload → slot swap → rollback-confirmation log (needs `build/pixfrog.bin`) |

Conventions (see `pixfrog_uart.py`):
- **One serial session per validator** — opening the port resets the board,
  so a validator never closes/reopens mid-run.
- Every validator **restores the board defaults** it touched (opt-in flags
  back to off, gamma to linear, …).
- UDP sends are **repeated/spread** — single datagrams behind a NAT
  routinely vanish on a cold ARP entry; treat one-packet tests as flaky.
- If `/dev/ttyACM0` vanishes after a replug, re-attach it to the test host
  (USB pass-through to a VM/container may need re-attaching).

Not covered here: wire-level timing/levels (Saleae workflows — see the
repo skills), sACN **multicast** (needs a sender directly on the board's
LAN — untestable from behind a NAT).

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

Which Saleae column carries which bus bit is wiring, not configuration —
establish it with `cal 1` (walking-1 across the 16 bits) rather than assuming.
`cal 0` (1 kHz square on all 16 GPIOs) is the fastest go/no-go: on a healthy
board every probed pin shows ~977 Hz.
