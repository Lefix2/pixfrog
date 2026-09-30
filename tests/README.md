# Host tests

Every host test runs from one CMake project, driven by ctest:

```bash
cmake -S tests -B build/tests && cmake --build build/tests --parallel
ctest --test-dir build/tests --output-on-failure        # all suites
build/tests/harness/harness_dmx_manager failsafe        # one binary, filtered cases
cmake -S tests -B build/tests-san -DPIXFROG_SANITIZE=ON # ASan + UBSan
python3 tools/coverage.py                               # whole-firmware coverage
```

## Layout

| Path | What |
|---|---|
| `components/*/test/` | Pure-logic suites (encoders, parsers, `dmx_logic.h`, layouts). Each still builds standalone. |
| `tests/shims/` | Host implementations of the IDF APIs the portable components call: fake clock (`esp_timer`, FreeRTOS ticks), in-memory NVS with fault injection, single-threaded FreeRTOS, real SHA-256, an in-process UDP fabric behind `lwip/sockets.h`, `esp_console` command table. Tests steer them through `shim_control.h`. |
| `tests/harness/` | The **real** firmware sources compiled against the shims — `config_store.cpp`, `dmx_manager.cpp`, `artnet.cpp`, `sacn.cpp`, `control_console.cpp`, `web_config.cpp` — one executable per area so their static state stays apart. `fakes/` stands in for modules not built here (FSEQ player, FPP, LED output, UI). |
| `tests/web/` | `pixfrog_api_host`: the real `web_config` handlers and gzipped SPA served over TCP on the host (`--port N [--rollback] [--password P]`), and Playwright tests driving the SPA in Chromium against it. |
| `tests/third_party/cJSON/` | cJSON as vendored by ESP-IDF (MIT), for the web handlers. |
| `tests/fuzz/` | libFuzzer targets (`-DPIXFROG_FUZZ=ON`, clang): the wire parsers, the real Art-Net/sACN receive loops, the console table, the REST handlers. `corpus/<target>/` holds the seeds (`make_seeds.py` regenerates them). |
| `tools/emulator/crawl.py` | Walks every menu node of the UI emulator and compares golden screenshots (both panels). |

## REST API and SPA

`harness_web` calls the handlers in process with `shim::http_request("POST",
"/api/scenes", body)` — contract tests on status codes, JSON shape and the
device state behind it. The browser tests need the API host built:

```bash
pip install pytest playwright && python3 -m playwright install chromium
cmake --build build/tests --target pixfrog_api_host
python3 -m pytest tests/web -q
```

`@pytest.mark.device_args("--rollback")` starts a test's device with extra
flags.

## Writing a harness test

```cpp
#include "harness.h"
#include "shim_control.h"

TEST(failsafe_after_timeout) {
    // …drive the component…
    shim::advance_ms(2100);  // time only moves when the test says so
    EXPECT_EQ(value, 0);
}
```

- `TEST(name)` self-registers; `EXPECT_TRUE/FALSE/EQ/STREQ` keep going on failure.
- A blocking call (`xSemaphoreTake`, `vTaskDelay`) advances the fake clock by
  its timeout instead of sleeping.
- `xTaskCreate*` records the task; `shim::run_task("artnet_rx")` runs its body
  on the test thread. A receive loop returns once its socket queue is empty if
  the test registered `shim::net_on_idle(port, stop_fn)`.
- `shim::console_exec("ch 0 pixels 60", &out)` runs a console line as the UART
  REPL would and captures its output.
- Prove a new test can fail: break the code it guards once (a quick mutation)
  and watch it go red — the bank-swap flicker and the failsafe-mask checks were
  verified that way.

## Fuzzing

```bash
tools/fuzz.sh 60                    # every target 60 s in parallel (CI job `fuzz`)
tools/fuzz.sh 600 fuzz_web_api      # one target, longer
build/tests-fuzz/fuzz/fuzz_web_api build/fuzz-artifacts/fuzz_web_api/crash-…  # replay
```

A crash leaves its input in `build/fuzz-artifacts/<target>/`. Turn it into a
harness test (see `out_of_range_numbers_are_ignored_not_wrapped` in
`test_web.cpp`, the first bug the fuzzer found), fix, and keep the input as a
seed. Under ctest the fuzz build only replays the seeds.

| Target | Input |
|---|---|
| `fuzz_parsers` | one buffer through every Art-Net, sACN, FPP and FSEQ-header parser |
| `fuzz_receivers` | byte 0 picks Art-Net/sACN and the source; datagrams split on `FF FE FD` |
| `fuzz_console` | one console line |
| `fuzz_web_api` | byte 0 route, byte 1 wildcard index + receive chunking, then the body |
