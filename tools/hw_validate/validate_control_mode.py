#!/usr/bin/env python3
"""Fixture control on the board: an output's fixtures driven like conventional
luminaires through their DMX profiles — alone (no pixel data): the patch
sheet, plain colours, an effect of the bank, dimmer, a profile edit moving the
addresses; then over the output's pixel mapping, each on its own universe: a
fixture's Effect channel at 0 shows its pixels under its dimmer, above 0 the
effect; then neither, and the way back to pixel mapping alone."""
import sys

from pixfrog_uart import Board, Checks, main_guard


def px(board, ch, first, count=1):
    """`count` pixels of channel `ch` from `first`, as a list of (r, g, b)."""
    d = board.get(f"pixr {ch} {first * 3} {count * 3}", "data") or ""
    return [tuple(bytes.fromhex(d[i:i + 6])) for i in range(0, len(d) - 5, 6)]


def run(board: Board):
    c = Checks("control_mode")
    board.cmd("scene stop")
    board.cmd("show fade 0")
    board.cmd("ch 0 protocol WS2815")
    board.cmd("ch 0 order RGB")
    board.cmd("ch 0 pixels 60")
    board.cmd("ch 0 universe 1")
    board.cmd("ch 0 dmx_start 1")
    for n, preset in enumerate(("rgb", "dim_rgb", "rgb_fx", "full")):
        board.cmd(f"profile preset {n} {preset}")
    # Three 20-pixel fixtures: RGB (3 ch), RGB FX (6 ch), Dim RGB (4 ch).
    board.cmd("ch 0 fixtures 1:20,21:20:p2,41:20:p1")
    board.cmd("ch 0 pixel_map 0")  # the fixtures alone, where the output was
    board.cmd("ch 0 fixture_ctl 1")
    board.cmd("ch 0 fix_universe 1")
    board.cmd("ch 0 fix_dmx 1")
    c.check("link up", board.wait_link())

    st = board.cmd("ch 0")
    c.check("patch sheet: the fixtures follow each other", board.kv(st, "patch") == "1.1+3,1.4+6,1.10+4")
    c.check("13 channels take one universe", board.kv(st, "universes") == "1")

    #          RGB       R G B  bank spd shut  dim R  G B
    frame = "0a141e" "00005a" "000000" "ff280000"
    board.cmd(f"dmxw 1 1 {frame}")
    board.cmd("status")
    c.check("fixture 1: its colour on every pixel", px(board, 0, 0, 20) == [(10, 20, 30)] * 20)
    c.check("fixture 2: plain blue, no effect", px(board, 0, 20, 20) == [(0, 0, 90)] * 20)
    c.check("fixture 3: red at full", px(board, 0, 40) == [(40, 0, 0)])

    board.cmd("dmxw 1 10 80")  # fixture 3's dimmer at half
    board.cmd("status")
    p = px(board, 0, 40)
    c.check(f"fixture 3 dimmed to half ({p})", p and abs(p[0][0] - 20) <= 1)

    # Effect 1 of the bank on fixture 2 — a steady colour, so it reads back
    # exactly — first in its own colour, then in the desk's.
    board.cmd("fx set 0 solid c86432 0 0")
    board.cmd("dmxw 1 4 000000080000")
    board.cmd("status")
    c.check("the effect channel plays effect 1 of the bank", px(board, 0, 20) == [(200, 100, 50)])
    board.cmd("dmxw 1 4 004600")
    board.cmd("status")
    c.check("a desk colour replaces the effect's", px(board, 0, 20) == [(0, 70, 0)])
    c.check("the other fixtures are untouched", px(board, 0, 0) == [(10, 20, 30)])

    # A profile edit moves the addresses of what follows.
    board.cmd("profile preset 0 full")  # fixture 1: 16 channels now
    st = board.cmd("ch 0")
    c.check("a longer profile pushes the next fixtures", board.kv(st, "patch") == "1.1+16,1.17+6,1.23+4")
    board.cmd("profile preset 0 rgb")

    # No fixture listed: the strip is one fixture on the first profile.
    board.cmd("ch 0 fixtures -")
    board.cmd("dmxw 1 1 112233")
    board.cmd("status")
    c.check("without fixtures the strip is one fixture",
            board.kv(board.cmd("ch 0"), "patch") == "1.1+3" and
            px(board, 0, 0) == [(17, 34, 51)] and px(board, 0, 59) == [(17, 34, 51)])

    # Both at once: the pixels on universe 1, the fixtures' channels on universe 2.
    board.cmd("ch 0 fixtures 1:20,21:20:p2,41:20:p1")
    board.cmd("ch 0 packing continuous")
    board.cmd("ch 0 pixel_map 1")
    board.cmd("ch 0 fix_universe 2")
    st = board.cmd("ch 0")
    c.check("both: the fixtures on their own universe", board.kv(st, "patch") == "2.1+3,2.4+6,2.10+4")
    c.check("both: one universe of pixels, one of fixtures", board.kv(st, "universes") == "2")
    for first in (0, 20, 40):  # one pixel of each fixture, and its neighbour dark
        board.cmd(f"dmxw 1 {first * 3 + 1} 203040000000")
    #                    RGB       R G B bank spd shut  dim R  G B
    board.cmd("dmxw 2 1 000000" "000000" "000000" "ff000000")
    board.cmd("status")
    c.check("fixture 1 (no dimmer): its pixels as sent", px(board, 0, 0, 2) == [(32, 48, 64), (0, 0, 0)])
    c.check("fixture 2, Effect at 0: its pixels", px(board, 0, 20, 2) == [(32, 48, 64), (0, 0, 0)])
    c.check("fixture 3, dimmer full: its pixels", px(board, 0, 40) == [(32, 48, 64)])
    board.cmd("dmxw 2 10 80")
    board.cmd("status")
    p = px(board, 0, 40)
    c.check(f"fixture 3's dimmer at half dims its pixels ({p})",
            p and all(abs(a - b) <= 1 for a, b in zip(p[0], (16, 24, 32))))
    board.cmd("dmxw 2 7 08")  # fixture 2's Effect channel: effect 1 of the bank
    board.cmd("status")
    c.check("Effect above 0: the effect replaces the fixture's pixels",
            px(board, 0, 20, 2) == [(200, 100, 50)] * 2)
    c.check("the other fixtures keep their pixels", px(board, 0, 0, 2) == [(32, 48, 64), (0, 0, 0)])
    board.cmd("dmxw 2 7 00")
    board.cmd("status")
    c.check("Effect back at 0: its pixels again", px(board, 0, 20, 2) == [(32, 48, 64), (0, 0, 0)])
    board.cmd("dmxw 2 4 0a141e03")  # R G B of fixture 2, Effect in the 1-7 band: the desk's plain colour
    board.cmd("status")
    c.check("Effect at 1-7: the desk's colour over the pixels", px(board, 0, 20, 2) == [(10, 20, 30)] * 2)
    c.check("the other fixtures keep their pixels", px(board, 0, 0, 2) == [(32, 48, 64), (0, 0, 0)])
    board.cmd("dmxw 2 4 00000000")
    board.cmd("dmxw 2 9 ff")  # fixture 2's shutter: strobing — dark part of the time
    seen = set()
    for _ in range(12):
        board.cmd("status")
        seen.add(tuple(px(board, 0, 20)))
    c.check(f"the shutter strobes the fixture's pixels ({len(seen)} states)",
            ((32, 48, 64),) in seen and ((0, 0, 0),) in seen)
    board.cmd("dmxw 2 9 00")

    # Neither: the output takes no universe (scenes still play on it).
    board.cmd("ch 0 pixel_map 0")
    board.cmd("ch 0 fixture_ctl 0")
    st = board.cmd("ch 0")
    c.check("neither: no universe, no patch sheet",
            board.kv(st, "universes") == "0" and board.kv(st, "patch") is None)
    board.cmd("status")
    c.check("neither: nothing of its own to show", px(board, 0, 0) == [(0, 0, 0)])

    # Back to pixel mapping alone: the same bytes are pixels again.
    board.cmd("ch 0 pixel_map 1")
    board.cmd("ch 0 fixtures -")
    st = board.cmd("ch 0")
    c.check("pixel mapping alone: one universe", board.kv(st, "universes") == "1")
    board.cmd("dmxw 1 1 112233445566")
    board.cmd("status")
    c.check("pixel mapping again", px(board, 0, 0, 2) == [(17, 34, 51), (68, 85, 102)])
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
