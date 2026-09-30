---
name: host-tests
description: Build and run every host test suite (ctest) — plain, under ASan+UBSan, and with the whole-firmware coverage report; no IDF needed
---

Every suite lives in one CMake project, `tests/CMakeLists.txt` (it aggregates
`components/*/test` plus the IDF-shim harness):

```bash
cmake -S tests -B build/tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests --parallel
ctest --test-dir build/tests --output-on-failure
```

Same suites under AddressSanitizer + UndefinedBehaviorSanitizer (CI job
`sanitizers`; any report fails the test):

```bash
cmake -S tests -B build/tests-san -DPIXFROG_SANITIZE=ON
cmake --build build/tests-san --parallel
ctest --test-dir build/tests-san --output-on-failure
```

Coverage (CI job `coverage`, uploaded to Codecov) — host suites + emulator
scenarios; firmware files no test compiles count at 0 %, so the figure is the
whole code base. Needs `pip install gcovr` (+ `libsdl2-dev` for the emulator):

```bash
python3 tools/coverage.py          # → build/coverage/{summary.txt,coverage.lcov,html/}
```

SPA in Chromium against the host API server (CI job `web-ui`):

```bash
pip install pytest playwright && python3 -m playwright install chromium
python3 -m pytest tests/web -q      # needs build/tests/harness/pixfrog_api_host
```

UI menu crawl + golden screenshots (CI job `emulator`, `pip install pillow`):
`python3 tools/emulator/crawl.py tools/emulator/build/pixfrog_emu` (see the
emulator skill; `--update-golden` after an intended UI change).

Fuzzing (CI job `fuzz`, clang): `tools/fuzz.sh [SECONDS] [target…]` — see
tests/README.md for the targets and how to turn a crash into a harness test.

Each suite prints `PASS=<n> FAIL=0`. A new suite is registered in
`tests/CMakeLists.txt` only — CI and `tools/ci-local.sh` pick it up.
