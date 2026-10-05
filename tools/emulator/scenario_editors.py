#!/usr/bin/env python3
"""Drives what the crawler only cancels or never reaches: commits the text,
IP and universe editors, the network/traffic states of HOME, the stats/about
pages, the FSEQ file browser on a fake SD card, and the show-control rows.
Asserts the committed values through the menu state.

    tools/emulator/scenario_editors.py [build/pixfrog_emu]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from crawl import Emu, goto, home  # noqa: E402

MAIN_CH1, MAIN_INPUTS, MAIN_NETWORK, MAIN_OUTPUT, MAIN_PLAYBACK = 0, 8, 9, 10, 11
MAIN_SETTINGS, MAIN_ABOUT = 12, 13
SETTINGS_STATS = 4  # after Bright, Idle dim, Dim after, Refresh px (no speaker here)


def expect(cond, what):
    if not cond:
        raise SystemExit(f"SCENARIO FAIL: {what}")


def click_until_leaves(emu, screen, limit=80):
    for _ in range(limit):
        emu.cmd("click")
        if emu.state()["screen"] != screen:
            return True
    return False


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "pixfrog_emu")
    emu = Emu(binary)
    for c in ("set chan 0 1 1 300", "set net connected", "set ip 192.168.2.50"):
        emu.cmd(c)
    try:
        # HOME in every network state and with big traffic counters.
        for net in ("acquiring", "error", "disconnected", "connected"):
            emu.cmd(f"set net {net}")
            emu.shot(os.devnull)
        for pkts in (2_500_000, 1_500_000_000, 42):
            emu.cmd(f"set pkts {pkts}")
            emu.shot(os.devnull)

        # Text editor: walk to the end and validate (Name).
        st = goto(emu, [0, MAIN_NETWORK, 5])
        expect(st["screen"] == "EditString", f"Name opens the text editor ({st})")
        emu.cmd("right")  # change the first character
        expect(click_until_leaves(emu, "EditString"), "text editor commits")
        # At the DONE position, rotate back once, then validate (Long).
        goto(emu, [0, MAIN_NETWORK, 6])
        for _ in range(70):
            emu.cmd("click")
            if emu.state()["screen"] != "EditString":
                break
        # IP editor: every octet, then commit (IP, then Mask with a step back).
        st = goto(emu, [0, MAIN_NETWORK, 1])
        expect(st["screen"] == "EditIp", "IP opens the address editor")
        emu.cmd("right")
        expect(click_until_leaves(emu, "EditIp"), "IP editor commits")
        goto(emu, [0, MAIN_NETWORK, 2])
        for _ in range(4):
            emu.cmd("click")
        emu.cmd("left")  # back from DONE to the last octet
        expect(click_until_leaves(emu, "EditIp"), "mask editor commits")
        # Universe editor on channel 1 (net.sub.uni segments).
        st = goto(emu, [0, MAIN_CH1, 1])
        expect(st["screen"] == "EditUni", f"Uni opens the universe editor ({st})")
        emu.cmd("right")
        for _ in range(3):
            emu.cmd("click")
        emu.cmd("left")
        expect(click_until_leaves(emu, "EditUni"), "universe editor commits")

        # Stats (under Settings) and About: a click returns to their menu.
        for path, screen, back in (([0, MAIN_SETTINGS, SETTINGS_STATS], "Stats", "SettingsMenu"),
                                   ([0, MAIN_ABOUT], "About", "MainMenu")):
            st = goto(emu, path)
            expect(st["screen"] == screen, f"{screen} opens")
            emu.cmd("click")
            expect(emu.state()["screen"] == back, f"{screen} click returns")

        # Sounds: what a press means (+1 confirm, -1 cancel) and the gauge level.
        home(emu)
        st = emu.state()
        expect(st["click"] == 1 and st["long"] == 0, f"HOME: a click confirms, no long sound ({st})")
        st = goto(emu, [0])
        expect(st["long"] == -1, "a long press in the menu cancels")
        for _ in range(MAIN_ABOUT + 1):
            emu.cmd("right")
        expect(emu.state()["click"] == -1, "the [Back] row cancels")
        st = goto(emu, [0, MAIN_SETTINGS, SETTINGS_STATS])
        expect(st["click"] == -1, "a click on Stats goes back: cancel")
        # Speaker: Volume appears in Settings, its gauge pitch follows the value.
        emu.cmd("set speaker 1")
        st = goto(emu, [0, MAIN_SETTINGS, SETTINGS_STATS])
        expect(st["screen"] == "EditValue", f"Volume sits before Nerd stats ({st})")
        expect(st["gauge"] == 0.0, f"volume off: the gauge at its low bound ({st})")
        before = st["fp"]
        emu.cmd("right")
        st = emu.state()
        expect(abs(st["gauge"] - 0.05) < 1e-3 and st["fp"] != before, f"a step moves it ({st})")
        expect(st["click"] == 1, "committing confirms")
        emu.cmd("click")
        st = goto(emu, [0, MAIN_SETTINGS, SETTINGS_STATS])  # back in: 5 % kept
        expect(abs(st["gauge"] - 0.05) < 1e-3, f"the volume was saved ({st})")
        emu.cmd("left")
        emu.cmd("click")
        emu.cmd("set speaker 0")

        # FSEQ browser on a fake SD card: play a file, see it starred, stop.
        emu.cmd("set sd 3")
        st = goto(emu, [0, MAIN_PLAYBACK, 2])  # Scenes, Edit scenes, FSEQ
        expect(st["screen"] == "FSeqMenu", "FSEQ node opens")
        emu.cmd("click")  # show1.fseq
        emu.shot(os.devnull)
        for _ in range(3):
            emu.cmd("right")
        emu.cmd("click")  # [Stop]
        emu.cmd("set sd 0")

        # Show control rows: blackout on (HOME shows BO), master below 100 %.
        goto(emu, [0, MAIN_OUTPUT, 4])  # Blackout: toggles on click
        home(emu)
        emu.shot(os.devnull)
        goto(emu, [0, MAIN_OUTPUT, 4])
        st = goto(emu, [0, MAIN_OUTPUT, 3])  # Master
        expect(st["screen"] == "EditValue", "Master opens a value editor")
        emu.cmd("left")
        emu.cmd("click")
        home(emu)
        emu.shot(os.devnull)

        # DMX control slots: the rows that follow the function — a master's
        # 16-bit switch, a colour's number.
        ctl = [0, MAIN_INPUTS, 6]  # Enabled, Universe, Address, Preset, then the slots
        st = goto(emu, ctl + [4, 2])  # slot 1 is a master
        expect(st["screen"] == "EditValue", f"16-bit opens an editor ({st})")
        emu.cmd("left")
        emu.cmd("click")
        st = goto(emu, ctl + [5, 0])  # slot 2: blackout → red
        expect(st["screen"] == "EditValue", f"Function opens an editor ({st})")
        for _ in range(5):
            emu.cmd("right")
        emu.cmd("click")
        st = goto(emu, ctl + [5, 2])
        expect(st["screen"] == "EditValue", f"a colour slot has a Colour # row ({st})")
        emu.cmd("right")
        emu.cmd("click")
    finally:
        emu.close()
    print("SCENARIO OK: editors")


if __name__ == "__main__":
    main()
