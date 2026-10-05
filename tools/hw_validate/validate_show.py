#!/usr/bin/env python3
"""Show control on the board: grand master / blackout / strobe (console and
ArtTrigger), scene zones and crossfade, and the DMX control universe driven by
real Art-Net packets (master, blackout, scene band, release on loss)."""
import json
import random
import sys
import time

from pixfrog_uart import (Board, Checks, artnet_dmx, artnet_trigger, http, main_guard,
                          prime_network, udp_send)

CTRL_UNI = 100


def px0(board):
    """First pixel of channel 0 as (r, g, b), or None."""
    d = board.get("pixr 0 0 3", "data")
    return tuple(bytes.fromhex(d)) if d and len(d) == 6 else None


def macro(sub):
    return artnet_trigger(sub, key=1)  # KeyMacro: 1 toggle, 2 on, 3 off


def desk(values, repeat=12):
    """Send the control universe a few times (ARP/NAT warm-up), then settle."""
    udp_send(artnet_dmx(CTRL_UNI, bytes(values)), 6454, repeat=repeat, gap=0.05)
    time.sleep(0.2)


def run(board: Board):
    c = Checks("show")
    board.cmd("ch 0 protocol WS2815")
    board.cmd("ch 0 universe 1")
    board.cmd("ch 0 pixels 60")
    board.cmd("ch 0 order RGB")
    board.cmd("scene stop")
    board.cmd("show fade 0")
    c.check("link up", board.wait_link())
    prime_network()

    # ── local master / blackout / strobe ────────────────────────────────────
    board.cmd("dmxw 1 1 c86432")  # 200,100,50
    board.cmd("status")
    c.check("live pixel reads back", px0(board) == (200, 100, 50))
    board.cmd("show master 50")
    p = px0(board)
    c.check(f"master 50 % halves the output ({p})", p and abs(p[0] - 100) <= 1 and abs(p[2] - 25) <= 1)
    board.cmd("show blackout on")
    c.check("blackout darkens", px0(board) == (0, 0, 0))
    board.cmd("show blackout off")
    board.cmd("show master 100")
    c.check("master 100 % and lit again", px0(board) == (200, 100, 50))
    board.cmd("show strobe 7")
    # A 30 ms flash every ~143 ms. One console round trip takes ~200 ms, so a
    # 5 Hz strobe was sampled at the same phase every time (always dark):
    # 7 Hz plus a random jitter walks the phase. Sample until both states show.
    lit = set()
    for _ in range(60):
        lit.add(px0(board) == (200, 100, 50))
        if lit == {True, False}:
            break
        time.sleep(random.uniform(0, 0.07))
    c.check("strobe alternates lit and dark", lit == {True, False})
    board.cmd("show strobe 0")

    udp_send(macro(2), 6454, repeat=5, gap=0.1)
    time.sleep(0.2)
    c.check("ArtTrigger KeyMacro 2 = blackout on", board.get("show", "blackout_local") == "ff")
    udp_send(macro(3), 6454, repeat=5, gap=0.1)
    time.sleep(0.2)
    c.check("ArtTrigger KeyMacro 3 = blackout off", board.get("show", "blackout_local") == "00")

    # ── zones + crossfade ───────────────────────────────────────────────────
    # Scene n plays effect n, a steady colour, on every output.
    board.cmd("fx set 0 solid ff0000 0 0")
    board.cmd("fx set 1 solid 0000ff 0 0")
    board.cmd("scene part 0 ff 0")
    board.cmd("scene part 1 ff 1")
    board.cmd("scene play 0 0f")
    board.cmd("scene play 1 f0")
    c.check("two scenes on two zones",
            board.get("scene", "outputs") == "0,0,0,0,1,1,1,1")
    board.cmd("scene stop")
    board.cmd("show fade 1500")
    board.cmd("scene play 1")
    time.sleep(0.5)
    mid = px0(board)
    time.sleep(1.4)
    end = px0(board)
    c.check(f"crossfade passes through a mix ({mid} → {end})",
            mid and end == (0, 0, 255) and 0 < mid[2] < 255)
    board.cmd("scene stop")
    board.cmd("show fade 0")

    # ── DMX control universe over Art-Net ───────────────────────────────────
    board.cmd("ctrl preset simple")
    board.cmd(f"ctrl universe {CTRL_UNI}")
    board.cmd("ctrl address 1")
    board.cmd("ctrl enable 1")
    time.sleep(0.3)
    # simple: 1-2 master(16), 3 blackout, 4 strobe, 5 scene, 6 fade
    desk([0x80, 0x00, 0, 0, 0, 0])
    c.check("control universe live", board.get("show", "control") == "live")
    board.cmd("dmxw 1 1 c86432")
    p = px0(board)
    c.check(f"desk master 50 % ({p})", p and abs(p[0] - 100) <= 1)
    desk([0xFF, 0xFF, 255, 0, 0, 0])
    c.check("desk blackout", px0(board) == (0, 0, 0))
    desk([0xFF, 0xFF, 0, 0, 16, 0])  # band 2 = scene 2 (index 1, blue)
    c.check("desk scene band starts scene 2", board.get("scene", "active") == "1")
    board.cmd("scene stop")
    desk([0xFF, 0xFF, 0, 0, 16, 0])
    c.check("a local stop holds while the desk band is unchanged",
            board.get("scene", "active") == "-1")
    desk([0x20, 0x00, 255, 0, 0, 0])  # dark + dim, then walk away
    time.sleep(3.5)
    c.check("desk silent → control idle", board.get("show", "control") == "idle")
    c.check("desk silent → blackout released", board.get("show", "blackout") == "00")
    c.check("desk silent → master back to 100",
            (board.get("show", "master") or "").startswith("100,"))

    # ── fixture profile (web) ───────────────────────────────────────────────
    web_was_on = board.get("global", "web_enabled") == "1"
    if not web_was_on:
        board.cmd("global web_enabled 1")
        time.sleep(1)
    code, body = http("/api/control/fixture")
    try:
        prof = json.loads(body)
        chans = prof["modes"][0]["channels"]
    except (ValueError, KeyError, IndexError):
        chans = []
    c.check(f"fixture profile lists the mode ({len(chans)} ch)",
            code == 200 and chans[:2] == ["Master", "Master fine"] and len(chans) == 6)
    if not web_was_on:
        board.cmd("global web_enabled 0")

    # ── restore ─────────────────────────────────────────────────────────────
    board.cmd("ctrl enable 0")
    board.cmd("scene stop")
    board.cmd("show master 100")
    board.cmd("show blackout off")
    board.cmd("show strobe 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
