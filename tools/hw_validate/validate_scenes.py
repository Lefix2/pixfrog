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
