#!/usr/bin/env python3
"""Standalone scenes: the effect bank's generators, scene parts (which effect on
which outputs), network priority, ArtTrigger, boot scene."""
import sys, time
from pixfrog_uart import Board, Checks, artnet_trigger, http, udp_send, main_guard

JSON_POST = ["-X", "POST", "-H", "Content-Type: application/json", "-d"]


def run(board: Board):
    c = Checks("scenes")
    board.cmd("ch 0 protocol WS2815"); board.cmd("ch 0 universe 1")
    board.cmd("ch 1 protocol WS2815"); board.cmd("ch 1 universe 10")
    c.check("link up", board.wait_link())
    # The board's own layout and looks must not show through: an even strip
    # without fixtures (put back at the end), effects without their layers.
    pixels0 = board.get("ch 0", "pixels") or "48"
    fixtures0 = board.get("ch 0", "fixtures") or "-"
    board.cmd("ch 0 pixels 48")
    board.cmd("ch 0 fixtures -")

    # Scene n plays effect n on every output; the looks are set on the effects.
    for n in (0, 1, 2):
        board.cmd(f"scene part {n} ff {n}")
        board.cmd(f"fx phaser {n} none 0 0 0 0 forward")
        board.cmd(f"fx invert {n} 0")
        board.cmd(f"fx matricks {n} 0 0 0")
    board.cmd("fx set 0 solid 102030 0 0")
    board.cmd("scene play 0")
    board.cmd("status")
    c.check("solid fill on ch0", board.get("pixr 0 0 6", "data") == "102030102030")
    c.check("solid fill on ch1", board.get("pixr 1 0 3", "data") == "102030")

    board.cmd("dmxw 1 1 000000")  # deterministic universe content for the mask test
    board.cmd("scene clear 0")
    board.cmd("scene part 0 02 0")  # one part: ch1 only
    board.cmd("status")
    c.check("ch0 outside the scene decodes (black)", board.get("pixr 0 0 3", "data") == "000000")
    c.check("ch1 in the scene keeps it", board.get("pixr 1 0 3", "data") == "102030")

    # Two parts: one scene, an effect per output group.
    board.cmd("fx set 1 solid 405060 0 0")
    board.cmd("scene part 0 01 1")
    board.cmd("scene play 0")
    board.cmd("status")
    c.check("part 2 plays its own effect on ch0", board.get("pixr 0 0 3", "data") == "405060")
    c.check("part 1 keeps its effect on ch1", board.get("pixr 1 0 3", "data") == "102030")
    c.check("the scene lists both parts", "parts=02:0:each,01:1:each" in board.cmd("scene"))

    board.cmd("scene part 0 ff 0")
    board.cmd("dmxw 1 1 aabbcc")
    board.cmd("status")
    c.check("scene overrides traffic", board.get("pixr 0 0 3", "data") == "102030")
    board.cmd("scene stop")
    board.cmd("status")
    c.check("stop resumes decode", board.get("pixr 0 0 3", "data") == "aabbcc")

    board.cmd("fx set 1 chase ff0000 30 3")
    board.cmd("scene play 1")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""  # full strip: head anywhere
    c.check("chase renders head+background", "ff0000" in d and "000000" in d)

    board.cmd("fx set 2 rainbow ffffff 25 1")  # the board's bank may not be the factory one
    board.cmd("scene play 2")
    board.cmd("status")
    d = board.get("pixr 0 0 30", "data") or ""
    c.check("rainbow varies", len({d[i:i + 6] for i in range(0, len(d), 6)}) >= 5)

    # Multi-colour effects: every colour of the list reaches the strip.
    board.cmd("fx set 1 blobs ff0000,0000ff 0 2")  # speed 0: frozen blobs
    board.cmd("scene play 1")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""
    px = [bytes.fromhex(d[i:i + 6]) for i in range(0, len(d) - 5, 6)]
    c.check("blobs render both colours", any(p[0] > 0x80 and p[2] < 0x40 for p in px) and
            any(p[2] > 0x80 and p[0] < 0x40 for p in px))

    # Solid strobe: speed 0 = colour 1 static, 255 = colour 2 static.
    board.cmd("fx set 0 solid 110000,002200 0 0")
    board.cmd("scene play 0")
    board.cmd("status")
    c.check("solid speed 0 = colour 1", board.get("pixr 0 0 3", "data") == "110000")
    board.cmd("fx set 0 solid 110000,002200 255 0")
    board.cmd("status")
    c.check("solid speed 255 = colour 2", board.get("pixr 0 0 3", "data") == "002200")
    board.cmd("scene stop")

    # Dimmer phaser: a still PWM spread once along the strip (reversed, so
    # the cycle runs up the strip) lights its first half only; inverted, the
    # other half.
    n_px = int(board.get("ch 0", "pixels") or 0)
    board.cmd("fx set 0 solid 808080 0 0")
    board.cmd("fx phaser 0 pwm 0 16 0 0 reverse")
    board.cmd("scene play 0")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""
    px = [d[i:i + 6] for i in range(0, len(d) - 5, 6)]
    lit = [p != "000000" for p in px]
    c.check("phaser: one half lit, one half dark",
            n_px >= 4 and len(px) == n_px and lit[0] and not lit[-1] and
            abs(sum(lit) - n_px / 2) <= 1)
    board.cmd("fx invert 0 1")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""
    px = [d[i:i + 6] for i in range(0, len(d) - 5, 6)]
    c.check("invert: the halves swap", len(px) == n_px and px[0] == "000000" and
            px[-1] == "808080")
    board.cmd("fx invert 0 0")
    # Wings 2: the pattern is drawn on half the strip and mirrored on the
    # other half, so the strip reads the same from both ends.
    board.cmd("fx matricks 0 0 0 2")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""
    px = [d[i:i + 6] for i in range(0, len(d) - 5, 6)]
    c.check("wings 2: the strip is its own mirror image",
            len(px) == n_px and n_px % 2 == 0 and px == px[::-1] and "808080" in px and
            "000000" in px)
    # Block 4: values come four pixels at a time.
    board.cmd("fx matricks 0 4 0 0")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""
    px = [d[i:i + 6] for i in range(0, len(d) - 5, 6)]
    c.check("block 4: pixels share a value four by four",
            len(px) == n_px and all(len(set(px[i:i + 4])) == 1 for i in range(0, n_px, 4)))
    board.cmd("fx matricks 0 0 0 0")
    board.cmd("fx phaser 0 none 0 0 0 0 forward")
    board.cmd("scene stop")

    # A scene on a fixture group: output 1 as two 4-LED bars, a group of the
    # second one. That bar plays the scene, the first keeps the output's live
    # data; the part's direction runs the effect from the far end.
    board.cmd("ch 0 fixtures 1:4,5:4")
    code, _ = http("/api/groups", *JSON_POST, '{"groups":[{"name":"HwBar","members":[[0,1]]}]}')
    c.check("a group is created over REST", code == 200)
    board.cmd("fx set 0 solid 102030 0 0")
    board.cmd("dmxw 1 1 " + "00" * 24)
    board.cmd("scene play 0 group 0")
    board.cmd("status")
    c.check("the group's bar plays the scene, the other stays live",
            board.get("pixr 0 0 24", "data") == "000000" * 4 + "102030" * 4)
    board.cmd("fx set 0 gradient ff0000,0000ff 0 1")
    board.cmd("scene part 0 ff 0 chain")
    board.cmd("status")
    fwd = board.get("pixr 0 12 12", "data") or ""
    board.cmd("scene part 0 ff 0 chain rev")
    board.cmd("status")
    rev = board.get("pixr 0 12 12", "data") or ""
    bar = lambda d: [d[i:i + 6] for i in range(0, len(d), 6)]
    c.check("the part's direction runs the group from the far end",
            len(fwd) == 24 and fwd != rev and bar(rev) == bar(fwd)[::-1])
    c.check("the scene lists its direction", ":chain:rev" in board.cmd("scene"))
    board.cmd("scene stop")
    board.cmd("status")
    c.check("stop gives the bar back to the output",
            board.get("pixr 0 12 12", "data") == "000000" * 4)
    board.cmd("scene part 0 ff 0")
    board.cmd("fx set 0 solid 102030 0 0")
    http("/api/groups", *JSON_POST, '{"groups":[]}')
    board.cmd("ch 0 fixtures -")

    # Scene list: add / rename / move / delete, persisted across a reboot.
    count = lambda: sum(1 for l in board.cmd("scene").splitlines()
                        if l.strip().startswith("scene") and "name=" in l)
    n0 = count()
    idx = board.get("scene add HwValidate", "index")
    c.check("scene add returns the new index", idx == str(n0))
    board.cmd(f"scene name {idx} HwMoved")
    board.cmd(f"scene move {idx} 0")
    c.check("moved scene is first", "name=HwMoved" in board.cmd("scene").split("scene0 ", 1)[-1]
            .splitlines()[0])
    c.check("reboot (scene list)", board.reboot_and_resync())
    c.check("list survives reboot", count() == n0 + 1)
    board.cmd("scene del 0")
    c.check("scene del restores the count", count() == n0)
    c.check("delete beyond the list is refused", "ERR" in board.cmd(f"scene del {n0 + 5}"))

    board.cmd("scene stop")
    udp_send(artnet_trigger(1), 6454, repeat=10, gap=0.15)
    time.sleep(0.3)
    c.check("ArtTrigger plays scene 0", board.get("scene", "active") == "0")
    udp_send(artnet_trigger(0), 6454, repeat=10, gap=0.15)
    time.sleep(0.3)
    c.check("ArtTrigger stop", board.get("scene", "active") == "-1")

    board.cmd("global boot_scene 1")
    c.check("reboot", board.reboot_and_resync())
    c.check("boot scene active", board.get("scene", "active") == "0")

    board.cmd("scene stop")
    board.cmd("global boot_scene 0")
    board.cmd("ch 1 protocol Off")
    board.cmd(f"ch 0 pixels {pixels0}")
    board.cmd(f"ch 0 fixtures {fixtures0}")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
