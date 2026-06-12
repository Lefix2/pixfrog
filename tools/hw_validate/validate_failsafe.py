#!/usr/bin/env python3
"""Signal-loss failsafe: never-active rule, colour fill, recovery, blackout,
hold — the full state machine on ch0 with 2 s timeout."""
import sys, time
from pixfrog_uart import Board, Checks, main_guard


def run(board: Board):
    c = Checks("failsafe")
    board.cmd("ch 0 protocol WS2815")
    board.cmd("ch 0 universe 1")
    board.cmd("global failsafe_mode color")
    board.cmd("global failsafe_timeout_s 2")
    board.cmd("global failsafe_color 204080")
    out = board.cmd("global")
    c.check("config readback", board.kv(out, "failsafe_mode") == "color"
            and board.kv(out, "failsafe_timeout_s") == "2")

    # Never-active since boot: reboot to clear activity, wait past timeout.
    c.check("reboot", board.reboot_and_resync())
    time.sleep(3)
    board.cmd("status")
    c.check("never-active stays black", board.get("pixr 0 0 3", "data") == "000000")

    board.cmd("dmxw 1 1 aabbcc")
    board.cmd("status")
    c.check("injected data decoded", board.get("pixr 0 0 3", "data") == "aabbcc")

    time.sleep(3.5)
    board.cmd("status")
    c.check("failsafe colour after timeout", board.get("pixr 0 0 6", "data") == "204080204080")
    chline = [l for l in board.cmd("chstat").splitlines() if l.startswith("ch0 ")]
    c.check("chstat failsafe=1", chline and "failsafe=1" in chline[0])

    board.cmd("dmxw 1 1 112233")
    board.cmd("status")
    c.check("recovery on new packet", board.get("pixr 0 0 3", "data") == "112233")

    board.cmd("global failsafe_mode blackout")
    time.sleep(3.5)
    board.cmd("status")
    c.check("blackout after timeout", board.get("pixr 0 0 3", "data") == "000000")

    board.cmd("dmxw 1 1 445566")
    board.cmd("global failsafe_mode hold")
    time.sleep(3.5)
    board.cmd("status")
    c.check("hold keeps last look", board.get("pixr 0 0 3", "data") == "445566")

    board.cmd("global failsafe_mode hold")
    board.cmd("global failsafe_timeout_s 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
