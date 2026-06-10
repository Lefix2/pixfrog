---
name: host-tests
description: Build and run the three pure-host unit test suites (led_protocols, dmx_manager, artnet) — no IDF needed
---

```bash
for t in components/led_protocols/test components/dmx_manager/test components/artnet/test; do
    cmake -S "$t" -B "$t/build" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$t/build" --parallel >/dev/null
done
./components/led_protocols/test/build/test_led_protocols
./components/dmx_manager/test/build/test_dmx_logic
./components/artnet/test/build/test_artnet_parser
```

Each prints `PASS=<n> FAIL=0` on success. Any change in `led_protocols`, `dmx_manager`, or `artnet` requires the matching suite green.
