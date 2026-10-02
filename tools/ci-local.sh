#!/usr/bin/env bash
# Replays every job of .github/workflows/ci.yml locally. AGENT.md rule: this
# must be green before any push.
#
# Inside the devcontainer (or any shell with idf.py on PATH) the IDF builds
# run natively; otherwise they fall back to the espressif/idf:v5.5 docker
# image — the exact image CI uses.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==[1/4] clang-format (CI: format-check) =="
git ls-files '*.cpp' '*.h' | xargs clang-format --dry-run -Werror --style=file
python3 tools/lint_atomics.py

echo "==[2/4] host unit tests (CI: host-tests, sanitizers, coverage) =="
cmake -S tests -B build/tests -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/tests --parallel >/dev/null
ctest --test-dir build/tests --output-on-failure
cmake -S tests -B build/tests-san -DPIXFROG_SANITIZE=ON >/dev/null
cmake --build build/tests-san --parallel >/dev/null
ctest --test-dir build/tests-san --output-on-failure
if command -v clang++ >/dev/null 2>&1; then
    tools/fuzz.sh "${PIXFROG_FUZZ_SECS:-15}"  # CI: fuzz (60 s per target there)
else
    echo "(fuzz skipped: needs clang)"
fi
if python3 -c "import gcovr" 2>/dev/null; then
    python3 tools/coverage.py >/dev/null && cat build/coverage/summary.txt
    # CI fails a PR that lowers coverage (tools/coverage_gate.py): same check on
    # the local lcov, against main's figure on Codecov (skipped offline).
    git fetch -q origin main 2>/dev/null || true
    python3 tools/coverage_gate.py --lcov build/coverage/coverage.lcov
else
    echo "(coverage skipped: pip install gcovr to replay the coverage job)"
fi

echo "==[3/4] UI emulator builds + smoke tests (CI: emulator) =="
cmake -S tools/emulator -B tools/emulator/build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build tools/emulator/build --parallel >/dev/null
./tools/emulator/smoke.sh
cmake -S tools/emulator -B tools/emulator/build.st7789 -DCMAKE_BUILD_TYPE=Release \
    -DPIXFROG_EMU_PANEL=st7789 >/dev/null
cmake --build tools/emulator/build.st7789 --parallel >/dev/null
./tools/emulator/smoke.sh build.st7789/pixfrog_emu
if python3 -c "import PIL" 2>/dev/null; then
    python3 tools/emulator/crawl.py tools/emulator/build/pixfrog_emu
    python3 tools/emulator/crawl.py tools/emulator/build.st7789/pixfrog_emu --panel st7789
else
    echo "(menu crawl skipped: pip install pillow)"
fi
if python3 -c "import playwright, pytest" 2>/dev/null; then
    echo "== web UI browser tests (CI: web-ui) =="
    python3 -m pytest tests/web -q
else
    echo "(web UI tests skipped: pip install pytest playwright && python3 -m playwright install chromium)"
fi

echo "==[4/4] IDF builds nv3007 (default) + st7789 + oled (CI: idf-build matrix) =="
# Separate build dir + sdkconfig per overlay: SDKCONFIG_DEFAULTS only applies
# when the sdkconfig file does not exist yet, so overlay builds must not
# share the default ./sdkconfig.
overlay_build() {  # $1 = variant name matching sdkconfig.ci.<variant>
    if command -v idf.py >/dev/null 2>&1; then
        idf.py -B "build.$1" \
            -D SDKCONFIG="build.$1/sdkconfig" \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.$1" \
            build
    else
        docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
            espressif/idf:v5.5 idf.py -B "build.$1" \
            -D SDKCONFIG="build.$1/sdkconfig" \
            -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.$1" \
            build
    fi
}
if command -v idf.py >/dev/null 2>&1; then
    idf.py build
else
    docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
        espressif/idf:v5.5 idf.py build
fi
overlay_build st7789
overlay_build oled

echo "ALL CI JOBS GREEN — safe to push"
