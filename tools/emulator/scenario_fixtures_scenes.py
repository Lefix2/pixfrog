#!/usr/bin/env python3
"""Drives the fixture and scene editors of the device menu and checks what
they stored (the emulator's `dump chan` / `dump scene`): the first fixture
taking the strip, a split, a fixture kept clear of its neighbours, the strip
growing under a new fixture, the DMX layout and a fixture's profile; a
scene's parts, their outputs handed from one part to the other, the effect,
fixture mode and direction, the group, the name, a new scene and its removal.

    tools/emulator/scenario_fixtures_scenes.py [build/pixfrog_emu]
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from crawl import Emu, goto  # noqa: E402
from scenario_editors import click_until_leaves, expect  # noqa: E402

CH1 = [0, 1, 0]  # main menu → Rig → output 1: Proto, Pixels, Dead px, Fixtures, …
CH_FIXTURES = 3
PATCH1 = [0, 3, 0, 0]  # main menu → DMX → Patch → output 1: Pixel map, Layout, Uni, DMX, Fixtures
SCENES = [0, 2, 0]  # main menu → Looks → Scenes (to edit)


def dump(emu, what):
    emu.cmd("dump " + what)
    while True:
        line = emu.p.stdout.readline()
        if not line:
            raise RuntimeError("emulator exited")
        if line.startswith("{"):
            return json.loads(line)


def edit_to(emu, path, lo, hi, target):
    """Open the numeric editor at `path` (range lo..hi) and commit `target`."""
    st = goto(emu, path)
    expect(st["screen"] == "EditValue", f"{path} opens a value editor ({st})")
    for _ in range(400):
        cur = round(lo + emu.state()["gauge"] * (hi - lo))
        if cur == target:
            break
        emu.cmd("right" if cur < target else "left")
    expect(round(lo + emu.state()["gauge"] * (hi - lo)) == target, f"{path} reaches {target}")
    emu.cmd("click")


def pick(emu, path, steps):
    """Open the list editor at `path`, turn it `steps` detents and commit."""
    st = goto(emu, path)
    expect(st["screen"] == "EditValue", f"{path} opens a list editor ({st})")
    for _ in range(abs(steps)):
        emu.cmd("right" if steps > 0 else "left")
    emu.cmd("click")


def fixtures(emu):
    fx = CH1 + [CH_FIXTURES]
    # An empty list: [Add], Split in, Back. The first fixture is the strip.
    st = goto(emu, fx + [1])
    expect(st["screen"] == "EditValue" and st["gauge"] == 0, f"Split in starts at one ({st})")
    emu.cmd("longclick")
    st = goto(emu, fx + [0])
    expect(st["screen"] == "FixtureMenu", f"[Add] opens the new fixture ({st})")
    expect(dump(emu, "chan 0")["fixtures"] == [[0, 300, 0, 0]], "the first fixture takes the strip")
    # F1, [Add], Split in: five equal fixtures replace it.
    edit_to(emu, fx + [2], 1, 32, 5)
    c = dump(emu, "chan 0")
    expect(c["fixtures"] == [[k * 60, 60, 0, 0] for k in range(5)], f"split in five ({c})")
    # Fixture 2: mounted the other way round, shortened, then moved — only as
    # far as fixture 3 lets it.
    pick(emu, fx + [1, 2], 1)
    edit_to(emu, fx + [1, 1], 1, 60, 50)
    edit_to(emu, fx + [1, 0], 61, 71, 71)
    expect(dump(emu, "chan 0")["fixtures"][1] == [70, 50, 1, 0], "fixture 2 reversed, 50 LEDs at 71")
    pick(emu, fx + [1, 2], 0)  # its Reversed row opens on ON
    expect(dump(emu, "chan 0")["fixtures"][1][2] == 1, "still reversed")
    # A sixth after the last: the strip grows to hold it.
    goto(emu, fx + [5])
    c = dump(emu, "chan 0")
    expect(c["pixels"] == 360 and c["fixtures"][5] == [300, 60, 0, 0], f"the strip grows ({c})")
    # The output's patch: two switches, each opening its own rows. Pixel map
    # (Layout, Uni, DMX) is on; Fixtures adds the fixtures' address and one
    # row a fixture, for its profile.
    pick(emu, PATCH1 + [1], 1)
    expect(dump(emu, "chan 0")["packing"] == 1, "layout: whole pixels")
    emu.cmd("set patch 0 1 0 2 9")  # a fixture address inside the output's own pixels (1-3)
    pick(emu, PATCH1 + [4], 1)
    c = dump(emu, "chan 0")
    expect(c["pixel_map"] and c["fixture_ctl"] and c["packing"] == 1, f"both ways at once ({c})")
    expect(c["fix"][0] > 3 and c["fix"][1] == 1, f"switched on over pixels: moved after them ({c})")
    emu.cmd("set patch 0 1 1 0 1")
    st = goto(emu, PATCH1 + [5])  # Fix uni: the net.sub.uni editor, on the fixtures' address
    expect(st["screen"] == "EditUni", f"Fix uni opens the universe editor ({st})")
    emu.cmd("click")
    emu.cmd("click")
    for _ in range(3):
        emu.cmd("right")
    expect(click_until_leaves(emu, "EditUni"), "the fixtures' universe commits")
    c = dump(emu, "chan 0")
    expect(c["fix"] == [3, 1] and c["uni"] == 1, f"fixtures on universe 3, pixels where they were ({c})")
    edit_to(emu, PATCH1 + [6], 1, 512, 21)
    expect(dump(emu, "chan 0")["fix"] == [3, 21], "fixtures from channel 21")
    st = goto(emu, PATCH1 + [7])  # … then F1
    expect(st["screen"] == "EditValue", f"a fixture row opens the profile picker ({st})")
    for _ in range(2):
        emu.cmd("right")
    emu.cmd("click")
    expect(dump(emu, "chan 0")["fixtures"][0] == [0, 60, 0, 2], "fixture 1 on the third profile")
    goto(emu, PATCH1[:-1])  # the list shows both universes
    # Pixel map off: the fixtures alone — Pixel map, Fixtures, Fix uni, Fix DMX, F1…
    pick(emu, PATCH1 + [0], -1)
    c = dump(emu, "chan 0")
    expect(not c["pixel_map"] and c["fixture_ctl"] and c["packing"] == 1, f"fixtures alone ({c})")
    st = goto(emu, PATCH1 + [4])
    expect(st["screen"] == "EditValue", f"F1 follows the fixtures' address ({st})")
    emu.cmd("longclick")
    goto(emu, PATCH1[:-1])
    # Fixtures off too: the output listens to no universe — two switches and Back.
    pick(emu, PATCH1 + [1], -1)
    c = dump(emu, "chan 0")
    expect(not c["pixel_map"] and not c["fixture_ctl"], f"neither ({c})")
    st = goto(emu, PATCH1 + [2])
    expect(st["screen"] == "PatchListMenu", f"only Back is left ({st})")
    pick(emu, PATCH1 + [0], 1)
    c = dump(emu, "chan 0")
    expect(c["pixel_map"] and c["packing"] == 1 and c["uni"] == 1, f"pixel map on: as it was ({c})")
    pick(emu, PATCH1 + [0], 1)  # already on: nothing moves
    expect(dump(emu, "chan 0")["packing"] == 1, "pixel map on twice: still the layout")
    st = goto(emu, PATCH1[:-1] + [7])  # output 8 is off: nothing to patch, Back only
    expect(st["screen"] == "OutputPatchMenu", f"the patch of an output that is off ({st})")
    emu.cmd("click")
    expect(emu.state()["screen"] == "PatchListMenu", "it only leads back")
    st = goto(emu, PATCH1 + [5])  # output 1's Back row (Pixel map, Layout, Uni, DMX, Fixtures, Back)
    expect(st["screen"] == "PatchListMenu", f"Back from the patch ({st})")
    pick(emu, PATCH1 + [0], -1)  # the fixtures alone again, for what follows
    pick(emu, PATCH1 + [1], 1)
    goto(emu, fx + [0, 3])  # [Delete]
    c = dump(emu, "chan 0")
    expect(len(c["fixtures"]) == 5 and c["fixtures"][0][0] == 70, f"fixture 1 deleted ({c})")
    for back in (fx + [0, 4], fx + [7]):  # the Back rows: a fixture's, the list's
        st = goto(emu, back)
        expect(st["screen"] in ("FixturesMenu", "ChannelMenu"), f"Back climbs a level ({st})")

    # A strip the output cannot drive any longer: [Add] gives the new fixture
    # the LEDs that are left, then none.
    emu.cmd("set chan 0 1 1 1000")
    emu.cmd("set fixtures 0 0:1000")
    goto(emu, fx + [1])
    c = dump(emu, "chan 0")
    expect(c["pixels"] == 1024 and c["fixtures"][1] == [1000, 24, 0, 0], f"the last 24 LEDs ({c})")
    goto(emu, fx + [2])
    expect(len(dump(emu, "chan 0")["fixtures"]) == 2, "no LED left: nothing added")
    # Room left on the strip: the next fixture just follows. Thirty-two fill the
    # list: [Add] is gone, Split in comes first after them.
    emu.cmd("set chan 0 1 1 300")
    emu.cmd("set fixtures 0 0:50")
    goto(emu, fx + [1])
    c = dump(emu, "chan 0")
    expect(c["pixels"] == 300 and c["fixtures"] == [[0, 50, 0, 0], [50, 50, 0, 0]], f"it fits ({c})")
    edit_to(emu, fx + [3], 1, 32, 32)
    st = goto(emu, fx + [32])
    expect(st["screen"] == "EditValue" and len(dump(emu, "chan 0")["fixtures"]) == 32,
           f"a full list has no [Add] ({st})")
    emu.cmd("longclick")
    # A fixture drawn past the strip's end keeps its length in the editors; one
    # whose profile left the bank is offered the first.
    emu.cmd("set chan 0 1 1 300")
    emu.cmd("set fixtures 0 0:400:p7")
    st = goto(emu, fx + [0, 1])
    expect(st["screen"] == "EditValue" and st["gauge"] > 0.99, f"400 LEDs is the top ({st})")
    emu.cmd("longclick")
    pick(emu, PATCH1 + [4], 0)
    expect(dump(emu, "chan 0")["fixtures"] == [[0, 400, 0, 0]], "the first profile")
    pick(emu, PATCH1 + [0], 1)  # pixel map alone again for the rest
    pick(emu, PATCH1 + [4], -1)
    c = dump(emu, "chan 0")
    expect(c["pixel_map"] and not c["fixture_ctl"], f"back to pixel mapping alone ({c})")
    # The list emptied from elsewhere while one of its fixtures is open: only
    # Back is left.
    goto(emu, fx + [0])
    emu.cmd("set fixtures 0")
    emu.cmd("click")
    expect(emu.state()["screen"] == "FixturesMenu", "a fixture that is gone leads back")


def scenes(emu):
    sc = SCENES + [0]  # scene 1: Name, Plays on, its part, [Play], [Stop], [Delete]
    expect(dump(emu, "scene 0")["parts"] == [[255, 0, 0, 0]], "scene 1 starts on every output")
    # Its part lets outputs 1 and 4 go...
    st = goto(emu, sc + [2, 0])
    expect(st["screen"] == "PartOutputsMenu", f"Outputs opens the toggles ({st})")
    emu.cmd("click")
    for _ in range(3):
        emu.cmd("right")
    emu.cmd("click")
    expect(dump(emu, "scene 0")["parts"] == [[0xF6, 0, 0, 0]], "outputs 1 and 4 left the part")
    # ... and [Add part] gives them to a second one, on its own effect, mirrored
    # and from the far end.
    st = goto(emu, sc + [3])
    expect(st["screen"] == "ScenePartMenu", f"[Add part] opens the new part ({st})")
    pick(emu, sc + [3, 1], 2)
    pick(emu, sc + [3, 2], 3)
    pick(emu, sc + [3, 3], 1)
    expect(dump(emu, "scene 0")["parts"] == [[0xF6, 0, 0, 0], [0x09, 2, 3, 1]], "part 2 as edited")
    # Output 2 moves over to part 2; a part's last output stays.
    goto(emu, sc + [3, 0])
    emu.cmd("right")
    emu.cmd("click")
    expect(dump(emu, "scene 0")["parts"][1][0] == 0x0B, "part 2 took output 2")
    expect(dump(emu, "scene 0")["parts"][0][0] == 0xF4, "part 1 lost it")
    goto(emu, sc + [2, 0])
    for o in range(8):  # every output of part 1 clicked: the last one holds
        goto(emu, sc + [2, 0])
        for _ in range(o):
            emu.cmd("right")
        if (0xF4 >> o) & 1:
            emu.cmd("click")
    p = dump(emu, "scene 0")["parts"]
    expect(len(p) == 2 and p[0][0] == 0x80, f"part 1 keeps its last output ({p})")
    # The default group, the name, a part removed.
    pick(emu, sc + [1], 1)
    expect(dump(emu, "scene 0")["group"] == 0, "the scene plays on the first group")
    st = goto(emu, sc + [0])
    expect(st["screen"] == "EditString", f"Name opens the text editor ({st})")
    emu.cmd("right")
    expect(click_until_leaves(emu, "EditString"), "the name commits")
    expect(dump(emu, "scene 0")["name"] != "Scene 1", "the name changed")
    pick(emu, sc + [3, 3], 0)  # part 2's Reverse row opens on ON
    goto(emu, sc + [2, 4])  # part 1 → [Delete]: part 2 closes up
    p = dump(emu, "scene 0")["parts"]
    expect(p == [[0x0B, 2, 3, 1]], f"part 1 deleted, part 2 takes its place ({p})")
    # [New] appends a scene and opens it; [Delete] removes it again.
    st = goto(emu, SCENES + [8])
    expect(st["screen"] == "SceneEditMenu", f"[New] opens the new scene ({st})")
    d = dump(emu, "scene 8")
    expect(d["count"] == 9 and d["parts"] == [[255, 0, 0, 0]], f"a new scene ({d})")
    goto(emu, SCENES + [8, 5])
    expect(dump(emu, "scene 0")["count"] == 8, "the new scene is deleted")
    # The last effect of the bank and the last group have no name: the pickers
    # number them.
    pick(emu, SCENES + [7, 1], 3)
    expect(dump(emu, "scene 7")["group"] == 2, "scene 8 on the unnamed third group")
    # Scene 8 plays an effect that left the bank: its editor offers the first.
    pick(emu, SCENES + [7, 2, 1], 0)
    expect(dump(emu, "scene 7")["parts"][0][1] == 0, "a missing effect becomes the first")
    # Back rows, from the innermost menu out (scene 1 has outputs to spare, so
    # an [Add part] row sits before its Play / Stop / Delete / Back).
    for back, where in ((SCENES + [0, 2, 0, 8], "ScenePartMenu"), (SCENES + [0, 2, 5], "SceneEditMenu"),
                        (SCENES + [0, 7], "SceneListMenu"), (SCENES + [9], "LooksMenu"),
                        (CH1 + [2, 1], "ChannelMenu")):  # and the dead-pixel list's
        st = goto(emu, back)
        expect(st["screen"] == where, f"Back from {back} lands on {where} ({st})")
    expect(dump(emu, "scene 0")["count"] == 8, "Back deletes nothing")
    # A scene deleted from elsewhere under the editors: each shows Back only.
    goto(emu, SCENES + [8])  # [New] → scene 9
    st = goto(emu, SCENES + [8, 2, 0])  # its part's outputs
    expect(st["screen"] == "PartOutputsMenu", f"scene 9's outputs ({st})")
    emu.cmd("set scenes 8")
    for where in ("ScenePartMenu", "SceneEditMenu", "SceneListMenu"):
        emu.cmd("click")
        st = emu.state()
        expect(st["screen"] == where, f"a scene that is gone leads back to {where} ({st})")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "pixfrog_emu")
    emu = Emu(binary)
    for c in ("set chan 0 1 1 300", "set net connected", "set ip 192.168.2.50"):
        emu.cmd(c)
    try:
        fixtures(emu)
        scenes(emu)
    finally:
        emu.close()
    print("SCENARIO OK")


if __name__ == "__main__":
    main()
