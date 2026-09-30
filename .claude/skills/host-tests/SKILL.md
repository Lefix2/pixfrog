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

Each suite prints `PASS=<n> FAIL=0`. A new suite is registered in
`tests/CMakeLists.txt` only — CI and `tools/ci-local.sh` pick it up.
