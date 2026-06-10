---
name: uartctl
description: Control the running firmware over UART — get/set any config field, telemetry, DMX injection, buffer readback
---

```bash
./tools/uartctl.sh -q "status" "ch 0" "global"        # -q = suppress device logs
```
Handles the open-port auto-reset (syncs on the `pixfrog>` prompt, ~4 s), prints `key=value` lines per command, exit 1 on any `ERR`/timeout. `PORT=/dev/ttyACMx` overrides.

Commands: `version status stats chstat` · `global [<key> <val>]` · `ch <n> [<key> <val>]` · `dmxw <uni> <slot> <hex>` · `dmxr <uni> [start len]` · `pixr <ch> [start len]` · `cal [-1..2]` · `loglevel <none..verbose>` · `factory-reset` · `reboot`. Key lists + ranges: AGENT.md "Control console (UART)".

End-to-end pipeline check (universe → decoded pixels):
```bash
./tools/uartctl.sh -q "ch 0 universe 1" "dmxw 1 1 ff0000" "pixr 0 0 3"   # expect data=ff0000
```
Config sets persist to NVS and apply next frame; network keys (`dhcp ip mask gw`) need `reboot`.
