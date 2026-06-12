---
name: hw-validate
description: Replayable hardware regression suite — run after any flash to prove every feature still works on the board
---

```bash
cd tools/hw_validate && ./run_all.py              # all but OTA (~3 min)
./run_all.py --with-ota                            # + OTA round-trip
./run_all.py scenes auth                           # subset
```
Validators: artnet, sacn, failsafe, scenes, identify_gamma, auth, ota.
Env: `PORT` (default /dev/ttyACM0), `BOARD_IP` (default 192.168.1.200),
`PIXFROG_BIN` for the OTA image (default `build/pixfrog.bin`).

Each validator holds ONE serial session (open = board reset, ~4 s sync) and
restores the defaults it touched. UDP tests repeat datagrams — the WSL2 NAT
eats single packets on cold ARP. Port gone? `usbipd.exe attach --wsl --busid <BUSID>`.
