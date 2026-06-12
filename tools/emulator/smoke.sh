#!/usr/bin/env bash
# Headless smoke test of the menu FSM through the stdin agent protocol.
# Exercises: splash skip, menu navigation, About screen, an EditValue
# commit via long-press, and long-press-as-Back — the paths that broke
# silently in the past. Used by ci.yml and ci-local.sh.
set -euo pipefail
cd "$(dirname "$0")"
BIN=${1:-build/pixfrog_emu}

out=$(printf '%s\n' \
    click state \
    right right right right right right right right right right right right right state \
    click state \
    longclick state \
    left left left left left left left left left left left left left \
    click click right right right state \
    longclick state \
    longclick longclick state \
    quit \
    | SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} timeout 60 "$BIN" --headless)

expect() {
    if ! grep -qF "$1" <<<"$out"; then
        echo "SMOKE FAIL: missing $1"
        echo "--- emulator output ---"
        echo "$out"
        exit 1
    fi
}

expect '"screen":"MainMenu","cursor":0'    # click on HOME opens the menu
expect '"screen":"MainMenu","cursor":13'   # 13 detents land on About
expect '"screen":"About"'                  # click enters About
expect '"screen":"EditValue"'              # ArtNet → Net edit
expect '"screen":"ArtnetMenu"'             # long-press commits the edit
expect '"screen":"Home"'                   # long-press × 2 climbs back home

echo "SMOKE OK"
