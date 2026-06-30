#!/usr/bin/env bash
# Headless smoke test of the menu FSM through the stdin agent protocol.
# Exercises: splash skip, menu navigation, the About screen, an EditValue
# commit, and long-press-as-Back — the paths that broke silently in the past.
# Used by ci.yml and ci-local.sh.
#
# Main menu layout (node engine): 0..7 channels, 8 Inputs, 9 Network,
# 10 Output, 11 Playback, 12 Nerd stats, 13 About, 14 [Back to HOME].
set -euo pipefail
cd "$(dirname "$0")"
BIN=${1:-build/pixfrog_emu}

out=$(printf '%s\n' \
    click state \
    right right right right right right right right right right right right state \
    click state \
    longclick \
    right state \
    click state \
    longclick \
    left left left left left state \
    click state \
    click state \
    longclick longclick longclick state \
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

expect '"screen":"MainMenu","cursor":0'   # click on HOME opens the menu
expect '"screen":"MainMenu","cursor":12'  # 12 detents land on Nerd stats
expect '"screen":"Stats"'                 # click enters the nerd-stats page
expect '"screen":"About"'                 # long-press back, +1 detent + click → About
expect '"screen":"InputsMenu"'            # back to menu, left ×5 + click → Inputs
expect '"screen":"EditValue"'             # Inputs → Net edit
expect '"screen":"Home"'                  # long-press climbs back: Inputs → Main → Home

echo "SMOKE OK"
