#!/bin/sh
# Builds tools/demo/preview.wasm — the firmware's effect engine for the demo
# box (preview_wasm.cpp) — with Emscripten (em++ on PATH, or EMSDK set).
#     tools/demo/build_wasm.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
if [ -n "$EMSDK" ] && [ -z "$(command -v em++)" ]; then . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1; fi
em++ -std=c++17 -Oz -fno-exceptions -fno-rtti -Wall -Wextra \
    -I "$REPO/components/config_store/include" \
    -I "$REPO/components/dmx_manager/src" \
    -I "$REPO/components/led_protocols/include" \
    -s STANDALONE_WASM -s PURE_WASI=0 --no-entry \
    -s EXPORTED_FUNCTIONS='["_pf_str","_pf_out","_pf_out_capacity","_pf_wave_from_id","_pf_fixture_mode_from_id","_pf_effect_begin","_pf_effect_color","_pf_effect_phaser","_pf_effect_matricks","_pf_render","_pf_strip_begin","_pf_strip_fixture","_pf_render_strip"]' \
    "$HERE/preview_wasm.cpp" -o "$HERE/preview.wasm"
ls -l "$HERE/preview.wasm"
