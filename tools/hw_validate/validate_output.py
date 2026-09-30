#!/usr/bin/env python3
"""Output timing and layout: refresh rate bounds (20..120 Hz) and the rate the
render loop actually holds, dead-pixel gaps (console round-trip, persistence,
logical pixels unaffected). The dark LEDs on the wire are checked with a
capture: nrz_decode.py --gaps (see README)."""
import sys
import time

from pixfrog_uart import Board, Checks, main_guard


def fps_after(board, hz, settle=3.0):
    board.cmd(f"global refresh_hz {hz}")
    time.sleep(settle)
    v = board.get("stats", "current_fps")
    return int(v) if v and v.isdigit() else -1


def run(board: Board):
    c = Checks("output")
    before_hz = board.get("global", "refresh_hz")
    board.cmd("ch 0 protocol WS2815")
    board.cmd("ch 0 universe 1")
    board.cmd("ch 0 pixels 60")
    board.cmd("ch 0 gaps -")

    # ── refresh bounds + held rate ──────────────────────────────────────────
    c.check("refresh 19 refused", "ERR" in board.cmd("global refresh_hz 19"))
    c.check("refresh 121 refused", "ERR" in board.cmd("global refresh_hz 121"))
    for hz in (20, 45, 120):
        fps = fps_after(board, hz)
        c.check(f"refresh {hz} Hz holds ({fps} fps, ±10 %)", abs(fps - hz) <= max(2, hz // 10))

    # A pixel count above the live budget is kept (non-destructive clamp).
    board.cmd("global refresh_hz 120")
    board.cmd("ch 0 pixels 1024")
    c.check("1024 px kept at 120 Hz", board.get("ch 0", "pixels") == "1024")
    board.cmd("ch 0 pixels 60")

    # ── dead-pixel gaps ─────────────────────────────────────────────────────
    c.check("gaps set", "OK" in board.cmd("ch 0 gaps 2:1,10:3"))
    c.check("gaps read back", board.get("ch 0", "gaps") == "2:1,10:3")
    c.check("overlapping gaps merged", "OK" in board.cmd("ch 0 gaps 20:2,21:3")
            and board.get("ch 0", "gaps") == "20:4")
    c.check("malformed gaps refused", "ERR" in board.cmd("ch 0 gaps 0:1"))
    board.cmd("ch 0 gaps 2:1,10:3")
    board.cmd("dmxw 1 1 aabbccddeeff")
    board.cmd("status")
    c.check("logical pixels unaffected by gaps",
            board.get("pixr 0 0 6", "data") == "aabbccddeeff")
    c.check("reboot (gaps)", board.reboot_and_resync())
    c.check("gaps survive reboot", board.get("ch 0", "gaps") == "2:1,10:3")
    fps = fps_after(board, 60)
    c.check(f"render holds 60 Hz with gaps ({fps} fps)", abs(fps - 60) <= 6)

    # ── restore ─────────────────────────────────────────────────────────────
    board.cmd("ch 0 gaps -")
    board.cmd(f"global refresh_hz {before_hz or 60}")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
