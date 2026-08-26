---
name: host-tests
description: Build and run the seven pure-host unit test suites (led_protocols, dmx_manager, artnet, config_store, sacn, fseq_player, fpp_sync) — no IDF needed
---

```bash
for t in components/led_protocols/test components/dmx_manager/test components/artnet/test \
         components/config_store/test components/sacn/test components/fseq_player/test \
         components/fpp_sync/test; do
    cmake -S "$t" -B "$t/build" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$t/build" --parallel >/dev/null
done
./components/led_protocols/test/build/test_led_protocols
./components/dmx_manager/test/build/test_dmx_logic
./components/artnet/test/build/test_artnet_parser
./components/config_store/test/build/test_config_store
./components/sacn/test/build/test_sacn_parser
./components/fseq_player/test/build/test_fseq_parser
./components/fpp_sync/test/build/test_fpp_sync_parser
```

Each prints `PASS=<n> FAIL=0` on success. These are the same seven suites the
`host-tests` CI job runs — keep this list in sync with `.github/workflows/ci.yml`
and `tools/ci-local.sh` when a component gains a suite.

A change in `led_protocols`, `dmx_manager`, or `artnet` requires the matching
suite green; the same goes for `config_store`, `sacn`, `fseq_player` and
`fpp_sync`.
