#!/usr/bin/env bash
# Headless smoke test of the menu FSM through the stdin agent protocol.
# Exercises: splash skip, menu navigation, the About screen, an EditValue
# commit, and long-press-as-Back — the paths that broke silently in the past.
# Used by ci.yml and ci-local.sh.
#
# Main menu layout (node engine): 0..7 channels, 8 Inputs, 9 Network,
# 10 Output, 11 Playback, 12 Display, 13 Nerd stats, 14 About,
# 15 [Back to HOME].
set -euo pipefail
cd "$(dirname "$0")"
BIN=${1:-build/pixfrog_emu}

out=$(printf '%s\n' \
    click state \
    right right right right right right right right right right right right right state \
    click state \
    longclick \
    right state \
    click state \
    longclick \
    left left left left left left state \
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
expect '"screen":"MainMenu","cursor":13'  # 13 detents land on Nerd stats
expect '"screen":"Stats"'                 # click enters the nerd-stats page
expect '"screen":"About"'                 # long-press back, +1 detent + click → About
expect '"screen":"InputsMenu"'            # back to menu, left ×5 + click → Inputs
expect '"screen":"EditValue"'             # Inputs → Net edit
expect '"screen":"Home"'                  # long-press climbs back: Inputs → Main → Home

# ── Backlight: the Display node previews the level live while editing ────────
out=$(printf '%s\n' \
    click \
    right right right right right right right right right right right right state \
    click click \
    left left left left left left state \
    quit \
    | SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} timeout 60 "$BIN" --headless)

expect '"screen":"MainMenu","cursor":12'  # 12 detents land on Display
expect '"backlight":70'                   # 6 detents down from 100 %, step 5

# ── Dim delay: the panel dims on tft_dim_delay_s, and the next event wakes it ─
# Real time, so the run is kept to the shortest settable delay (5 s, step 5).
out=$( {
    printf '%s\n' \
        click \
        right right right right right right right right right right right right \
        click right right click \
        left left left left left \
        click state \
        longclick longclick state
    sleep 7                      # 5 s delay + the 400 ms fade + slack
    printf '%s\n' state click state quit
} | SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} timeout 60 "$BIN" --headless)

expect '"screen":"DisplayMenu","cursor":2'  # commit lands back on "Dim after"
expect '"screen":"Home","cursor":0,"channel":0,"backlight":40'  # idle: -60 % of 100
# ...and the event after that is spent waking the panel, not on the menu.
if ! tail -1 <<<"$out" | grep -qF '"screen":"Home","cursor":0,"channel":0,"backlight":100'; then
    echo "SMOKE FAIL: the event after dimming did not restore full brightness on HOME"
    echo "--- emulator output ---"
    echo "$out"
    exit 1
fi

echo "SMOKE OK"
