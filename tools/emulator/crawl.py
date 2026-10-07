#!/usr/bin/env python3
"""Menu crawler for the headless emulator: visits every node, activates every
row, commits value edits and cancels the other editors, and checks that each
menu node and edit screen was reached. Selected screens are compared pixel for
pixel with golden images (tools/emulator/golden/<panel>/).

    tools/emulator/crawl.py [build/pixfrog_emu] [--panel nv3007|st7789]
    tools/emulator/crawl.py ... --update-golden     # after an intended UI change
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# Every NodeId (menu_debug_state names) plus the editor kinds must be reached.
REQUIRED = {
    "MainMenu", "ShowMenu", "LooksMenu", "RigMenu", "DmxMenu", "BoxMenu",
    "NetworkMenu", "SettingsMenu", "ChannelMenu", "ScenesMenu", "FSeqMenu", "TestPatternMenu",
    "GapsMenu", "Stats", "About", "EditValue", "EditString", "EditIp", "EditUni", "ControlMenu",
    "ControlSlotMenu", "FixturesMenu", "FixtureMenu", "SceneListMenu", "SceneEditMenu",
    "ScenePartMenu", "PartOutputsMenu", "PatchListMenu", "OutputPatchMenu",
}
TFT_ONLY = set()

# Golden screens: path of (cursor) clicks from HOME, taken on the seeded state
# before the crawl edits anything. Only static screens — nothing that shows uptime or live counters.
GOLDEN = {
    "main_menu": [0],
    "show_menu": [0, 0],
    "rig_menu": [0, 1],
    "channel1": [0, 1, 0],
    "channel1_dead_px": [0, 1, 0, 2],
    "channel1_fixtures": [0, 1, 0, 3],
    "channel1_fixture2": [0, 1, 0, 3, 1],
    "scenes_menu": [0, 0, 4],
    "scene_edit": [0, 2, 0, 1],
    "scene_part": [0, 2, 0, 1, 2],
    "dmx_menu": [0, 3],
    "patch_list": [0, 3, 0],
    "output_patch1": [0, 3, 0, 0],
    "output_patch2": [0, 3, 0, 1],  # pixels and fixtures at once
    "control_menu": [0, 3, 1],
}

SEED = [
    "set chan 0 1 1 300",    # WS2815
    "set chan 1 2 3 150",    # WS2812B
    "set chan 2 4 5 60",     # SK6812 (RGBW)
    "set chan 3 6 9 144",    # APA102 (clocked → Clock row)
    "set chan 4 8 11 64",    # LPD8806
    "set chan 5 7 13 100",   # SK9822
    "set chan 6 5 15 80",    # WS2814
    "set chan 7 0 17 10",    # Off
    "set gaps 0 10:2 40:1",
    "set fixtures 0 0:100 100:100:r 200:100",
    "set fixtures 1 0:75:p1 75:75:p2",
    "set patch 1 1 1 20 101",  # output 2: its pixels and its fixtures, each on its range
    "set patch 2 0 1 21 1",    # output 3: its fixtures alone
    "set patch 3 0 0",         # output 4: neither — scenes only
    "set net connected",
    "set ip 192.168.2.50",
]


class Emu:
    def __init__(self, binary):
        env = dict(os.environ, SDL_VIDEODRIVER=os.environ.get("SDL_VIDEODRIVER", "dummy"))
        self.p = subprocess.Popen([binary, "--headless"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, env=env, bufsize=1)
        self.steps = 0

    def cmd(self, c):
        self.p.stdin.write(c + "\n")
        self.steps += 1

    def state(self):
        self.cmd("state")
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("emulator exited")
            line = line.strip()
            if line.startswith("{"):
                return json.loads(line)
            if line.startswith("error"):
                raise RuntimeError(line)

    def shot(self, path):
        self.cmd("shot " + path)
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("emulator exited")
            if line.startswith("ok shot") or line.startswith("error"):
                return

    def close(self):
        self.cmd("quit")
        self.p.stdin.close()
        self.p.wait(timeout=10)


def home(emu):
    for _ in range(12):
        if emu.state()["screen"] == "Home":
            return
        emu.cmd("longclick")
    raise RuntimeError("could not get back HOME")


def goto(emu, path):
    """Replay `path` (one cursor per level) from HOME."""
    home(emu)
    for cur in path:
        for _ in range(60):
            emu.cmd("left")
        for _ in range(cur):
            emu.cmd("right")
        emu.cmd("click")
    return emu.state()


def count_rows(emu):
    for _ in range(60):
        emu.cmd("left")
    last = -1
    for _ in range(80):
        c = emu.state()["cursor"]
        if c == last:
            return c + 1
        last = c
        emu.cmd("right")
    return last + 1


def crawl(emu, seen):
    menus = [("MainMenu", [0])]  # (name, path) queue
    explored = set()
    # A menu is walked once — its rows are the same wherever it is opened
    # from — but for the channel menus, whose rows follow the protocol, and the
    # control slots, whose rows follow the function.
    every_time = ("ChannelMenu", "GapsMenu", "ControlSlotMenu")
    while menus:
        name, path = menus.pop(0)
        key = (name, tuple(path))
        st = goto(emu, path)
        again = name not in every_time and name in {n for n, _ in explored}
        if st["screen"] != name or key in explored or again or len(path) > 6:
            continue
        explored.add(key)
        seen.add(name)
        rows = count_rows(emu)
        for i in range(rows):
            st = goto(emu, path + [i])
            scr = st["screen"]
            seen.add(scr)
            if scr.startswith("Edit"):
                # One step up in the end — and a switch that is on stays on,
                # or the rows it opens would never be walked.
                for c in ("right", "left", "right"):
                    emu.cmd(c)
                if scr == "EditValue":
                    emu.cmd("click")  # commit: exercises commit_edit
                    after = emu.state()["screen"]
                    seen.add(after)
                    if after.startswith("Edit"):  # chained editor (dead px length)
                        emu.cmd("click")
                else:
                    emu.cmd("longclick")  # cancel
            elif scr.endswith("Menu") and scr != name and scr != "MainMenu":
                # Only descend: a Back row lands on the parent (already queued).
                if scr in every_time or scr not in {n for n, _ in explored}:
                    menus.append((scr, path + [i]))
            elif scr in ("Stats", "About", "PixelRefresh"):
                emu.cmd("longclick")
    return explored


def compare_golden(emu, panel, update):
    from PIL import Image, ImageChops  # pillow

    gdir = os.path.join(HERE, "golden", panel)
    os.makedirs(gdir, exist_ok=True)
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, path in GOLDEN.items():
            goto(emu, path)
            bmp = os.path.join(tmp, name + ".bmp")
            emu.shot(bmp)
            img = Image.open(bmp).convert("RGB")
            ref = os.path.join(gdir, name + ".png")
            if update or not os.path.exists(ref):
                img.save(ref)
                continue
            if ImageChops.difference(img, Image.open(ref).convert("RGB")).getbbox():
                out = os.path.join(tempfile.gettempdir(), f"golden-{panel}-{name}.png")
                img.save(out)
                bad.append(f"{name} (got {out})")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("binary", nargs="?", default=os.path.join(HERE, "build", "pixfrog_emu"))
    ap.add_argument("--panel", default="nv3007", choices=["nv3007", "st7789"])
    ap.add_argument("--update-golden", action="store_true")
    ap.add_argument("--no-golden", action="store_true")
    args = ap.parse_args()

    emu = Emu(args.binary)
    for c in SEED:
        emu.cmd(c)
    seen = set()
    try:
        # Goldens first: they show the seeded state, not what the crawl edited.
        bad = [] if args.no_golden else compare_golden(emu, args.panel, args.update_golden)
        explored = crawl(emu, seen)
    finally:
        emu.close()

    required = REQUIRED | TFT_ONLY
    missing = sorted(required - seen)
    print(f"crawl: {len(explored)} menus, {len(seen)} screens, {emu.steps} commands")
    ok = True
    if missing:
        print("CRAWL FAIL: never reached " + ", ".join(missing))
        ok = False
    if bad:
        print("GOLDEN FAIL: " + "; ".join(bad))
        ok = False
    if ok:
        print("CRAWL OK")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
