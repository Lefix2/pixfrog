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
| `auth` | open-by-default, 401s, flat brute-force delay, UART recovery |
| `ota` | upload → slot swap → rollback-confirmation log (needs `build/pixfrog.bin`) |

Conventions (see `pixfrog_uart.py`):
- **One serial session per validator** — opening the port resets the board,
  so a validator never closes/reopens mid-run.
- Every validator **restores the board defaults** it touched (opt-in flags
  back to off, gamma to linear, …).
- UDP sends are **repeated/spread** — single datagrams from a WSL2 NAT
  routinely vanish on a cold ARP entry; treat one-packet tests as flaky.
- WSL2: if `/dev/ttyACM0` vanished, re-attach from Windows:
  `usbipd.exe attach --wsl --busid <BUSID>`.

Not covered here: wire-level timing/levels (Saleae workflows — see the
repo skills), sACN **multicast** (untestable through the WSL2 NAT; needs a
sender on the board's LAN).
