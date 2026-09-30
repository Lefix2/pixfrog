#!/usr/bin/env python3
"""Standalone scenes: generators, channel mask, network priority, ArtTrigger,
boot scene."""
import sys, time
from pixfrog_uart import Board, Checks, artnet_trigger, udp_send, main_guard


def run(board: Board):
    c = Checks("scenes")
    board.cmd("ch 0 protocol WS2815"); board.cmd("ch 0 universe 1")
    board.cmd("ch 1 protocol WS2815"); board.cmd("ch 1 universe 10")
    c.check("link up", board.wait_link())

    board.cmd("scene set 0 solid 102030 0 0 ff")
    board.cmd("scene play 0")
    board.cmd("status")
    c.check("solid fill on ch0", board.get("pixr 0 0 6", "data") == "102030102030")
    c.check("solid fill on ch1", board.get("pixr 1 0 3", "data") == "102030")

    board.cmd("dmxw 1 1 000000")  # deterministic universe content for the mask test
    board.cmd("scene set 0 solid 102030 0 0 02")  # mask = ch1 only
    board.cmd("status")
    c.check("masked-out ch0 decodes (black)", board.get("pixr 0 0 3", "data") == "000000")
    c.check("masked-in ch1 keeps scene", board.get("pixr 1 0 3", "data") == "102030")

    board.cmd("scene set 0 solid 102030 0 0 ff")
    board.cmd("dmxw 1 1 aabbcc")
    board.cmd("status")
    c.check("scene overrides traffic", board.get("pixr 0 0 3", "data") == "102030")
    board.cmd("scene stop")
    board.cmd("status")
    c.check("stop resumes decode", board.get("pixr 0 0 3", "data") == "aabbcc")

    board.cmd("scene set 1 chase ff0000 60 3 ff")
    board.cmd("scene play 1")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""  # full strip: head anywhere
    c.check("chase renders head+background", "ff0000" in d and "000000" in d)

    board.cmd("scene play 2")
    board.cmd("status")
    d = board.get("pixr 0 0 30", "data") or ""
    c.check("rainbow varies", len({d[i:i + 6] for i in range(0, len(d), 6)}) >= 5)

    # Multi-colour effects: every colour of the list reaches the strip.
    board.cmd("scene set 1 blobs ff0000,0000ff 0 2 ff")  # speed 0: frozen blobs
    board.cmd("scene play 1")
    board.cmd("status")
    d = board.get("pixr 0", "data", deadline=8) or ""
    px = [bytes.fromhex(d[i:i + 6]) for i in range(0, len(d) - 5, 6)]
    c.check("blobs render both colours", any(p[0] > 0x80 and p[2] < 0x40 for p in px) and
            any(p[2] > 0x80 and p[0] < 0x40 for p in px))

    # Solid strobe: speed 0 = colour 1 static, 255 = colour 2 static.
    board.cmd("scene set 0 solid 110000,002200 0 0 ff")
    board.cmd("scene play 0")
    board.cmd("status")
    c.check("solid speed 0 = colour 1", board.get("pixr 0 0 3", "data") == "110000")
    board.cmd("scene set 0 solid 110000,002200 255 0 ff")
    board.cmd("status")
    c.check("solid speed 255 = colour 2", board.get("pixr 0 0 3", "data") == "002200")
    board.cmd("scene stop")

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
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
