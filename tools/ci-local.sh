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

echo "==[2/4] host unit tests (CI: host-tests) =="
for t in components/led_protocols/test components/dmx_manager/test components/artnet/test components/config_store/test components/sacn/test components/fseq_player/test components/fpp_sync/test; do
    cmake -S "$t" -B "$t/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$t/build" --parallel >/dev/null
done
./components/led_protocols/test/build/test_led_protocols
./components/dmx_manager/test/build/test_dmx_logic
./components/artnet/test/build/test_artnet_parser
./components/config_store/test/build/test_config_store
./components/sacn/test/build/test_sacn_parser
./components/fseq_player/test/build/test_fseq_parser
./components/fpp_sync/test/build/test_fpp_sync_parser

echo "==[3/4] UI emulator build + smoke test (CI: emulator) =="
cmake -S tools/emulator -B tools/emulator/build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build tools/emulator/build --parallel >/dev/null
./tools/emulator/smoke.sh

echo "==[4/4] IDF builds oled + tft (CI: idf-build matrix) =="
# Separate build dir + sdkconfig per overlay: SDKCONFIG_DEFAULTS only applies
# when the sdkconfig file does not exist yet, so overlay builds must not
# share the default ./sdkconfig.
if command -v idf.py >/dev/null 2>&1; then
    idf.py build
    idf.py -B build.oled \
        -D SDKCONFIG=build.oled/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.oled" \
        build
else
    docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
        espressif/idf:v5.5 idf.py build
    docker run --rm -v "$PWD":/project -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp \
        espressif/idf:v5.5 idf.py -B build.oled \
        -D SDKCONFIG=build.oled/sdkconfig \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.oled" \
        build
fi

echo "ALL CI JOBS GREEN — safe to push"
