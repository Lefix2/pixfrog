#!/usr/bin/env bash
# Headless smoke test of the menu FSM through the stdin agent protocol.
# Exercises: splash skip, menu navigation, the About screen, an EditValue
# commit, and long-press-as-Back — the paths that broke silently in the past.
# Used by ci.yml and ci-local.sh.
#
# Main menu layout (node engine): 0 Show, 1 Looks, 2 Rig, 3 DMX, 4 Box,
# 5 [Back to HOME]. Box: 0 Network, 1 Settings, 2 About, 3 [Back].
# Settings (TFT, no speaker): 0 Bright, 1 Idle dim, 2 Dim after,
# 3 Refresh px, 4 Nerd stats, 5 [Back]. DMX: 0 Patch (outputs 1-8).
set -euo pipefail
cd "$(dirname "$0")"
BIN=${1:-build/pixfrog_emu}

out=$(printf '%s\n' \
    click state \
    right right right right state \
    click right state \
    click right right right right state \
    click state \
    longclick longclick \
    right state \
    click state \
    longclick longclick \
    left state \
    click state \
    click state \
    click state \
    longclick longclick longclick longclick state \
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

expect '"screen":"MainMenu","cursor":0'      # click on HOME opens the menu
expect '"screen":"MainMenu","cursor":4'      # 4 detents land on Box
expect '"screen":"BoxMenu","cursor":1'       # in Box, 1 detent lands on Settings
expect '"screen":"SettingsMenu","cursor":4'  # in Settings, 4 detents land on Nerd stats
expect '"screen":"Stats"'                    # click enters the nerd-stats page
expect '"screen":"About"'                    # back ×2 (to Box), +1 detent + click → About
expect '"screen":"MainMenu","cursor":3'      # back ×2 (About, Box), left: DMX
expect '"screen":"DmxMenu"'                  # click → DMX
expect '"screen":"PatchListMenu"'            # click → Patch
expect '"screen":"OutputPatchMenu"'          # Patch → output 1
expect '"screen":"Home"'                     # long-press climbs back: output → Patch → DMX → Main → Home

# ── Backlight: the Settings node previews the level live while editing ────────
out=$(printf '%s\n' \
    click \
    right right right right click right state \
    click click \
    left left left left left left state \
    quit \
    | SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} timeout 60 "$BIN" --headless)

expect '"screen":"BoxMenu","cursor":1'  # Box, 1 detent: Settings
expect '"backlight":70'                 # 6 detents down from 100 %, step 5

# ── Dim delay: the panel dims on tft_dim_delay_s, and the next event wakes it ─
# Real time, so the run is kept to the shortest settable delay (5 s, step 5).
out=$( {
    printf '%s\n' \
        click \
        right right right right click right \
        click right right click \
        left left left left left \
        click state \
        longclick longclick longclick state
    sleep 7                      # 5 s delay + the 400 ms fade + slack
    printf '%s\n' state click state quit
} | SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} timeout 60 "$BIN" --headless)

expect '"screen":"SettingsMenu","cursor":2'  # commit lands back on "Dim after"
expect '"screen":"Home","cursor":0,"channel":0,"backlight":40'  # idle: -60 % of 100
# ...and the event after that is spent waking the panel, not on the menu.
if ! tail -1 <<<"$out" | grep -qF '"screen":"Home","cursor":0,"channel":0,"backlight":100'; then
    echo "SMOKE FAIL: the event after dimming did not restore full brightness on HOME"
    echo "--- emulator output ---"
    echo "$out"
    exit 1
fi

echo "SMOKE OK"
