#!/usr/bin/env python3
"""OTA: upload the current build to the inactive slot, verify the swap and
the rollback-confirmation log."""
import sys, time
from pixfrog_uart import Board, Checks, http, fw_bin, main_guard


def run(board: Board):
    c = Checks("ota")
    board.cmd("global web_enabled 1")
    c.check("link up", board.wait_link())

    before = board.get("version", "partition")
    code, body = http("/api/ota", "--data-binary", "@" + fw_bin())
    c.check("upload accepted", code == 200 and '"ok":true' in body)

    c.check("OTA image confirmed in boot log", board.watch_log("OTA image confirmed"))
    c.check("resync", board.sync())
    after = board.get("version", "partition")
    c.check(f"partition swapped ({before} → {after})", after and after != before)

    board.cmd("global web_enabled 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
