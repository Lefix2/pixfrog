#!/usr/bin/env bash
# Replays every job of .github/workflows/ci.yml locally. AGENT.md rule: this
# must be green before any push.
#
# Inside the devcontainer (or any shell with idf.py on PATH) the IDF builds
# run natively; otherwise they fall back to the espressif/idf:v5.5 docker
# image — the exact image CI uses.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==[1/3] clang-format (CI: format-check) =="
git ls-files '*.cpp' '*.h' | xargs clang-format --dry-run -Werror --style=file

echo "==[2/3] host unit tests (CI: host-tests) =="
for t in components/led_protocols/test components/dmx_manager/test components/artnet/test; do
    cmake -S "$t" -B "$t/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$t/build" --parallel >/dev/null
done
./components/led_protocols/test/build/test_led_protocols
./components/dmx_manager/test/build/test_dmx_logic
./components/artnet/test/build/test_artnet_parser

echo "==[3/3] IDF builds oled + tft + lcdcam (CI: idf-build matrix) =="
# Separate build dir + sdkconfig per overlay: SDKCONFIG_DEFAULTS only applies
# when the sdkconfig file does not exist yet, so overlay builds must not
# share the default ./sdkconfig.
if command -v idf.py >/dev/null 2>&1; then
    idf.py build
    idf.py -B build.tft \
        -D SDKCONFIG=build.tft/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.tft" \
        build
    idf.py -B build.lcdcam \
        -D SDKCONFIG=build.lcdcam/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.lcdcam" \
        build
else
    docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
        espressif/idf:v5.5 idf.py build
    docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
        espressif/idf:v5.5 idf.py -B build.tft \
        -D SDKCONFIG=build.tft/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.tft" \
        build
    docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
        espressif/idf:v5.5 idf.py -B build.lcdcam \
        -D SDKCONFIG=build.lcdcam/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.lcdcam" \
        build
fi

echo "ALL CI JOBS GREEN — safe to push"
