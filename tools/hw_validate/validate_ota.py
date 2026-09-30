#!/usr/bin/env python3
"""OTA: upload the current build to the inactive slot, verify the swap and the
delayed confirmation (30 s of healthy rendering); then the rollback round-trip:
upload again, reset before the confirmation, and check the bootloader went
back, the rollback was recorded (console + /api/status) and can be
acknowledged from the web."""
import json
import sys
import time

from pixfrog_uart import Board, Checks, fw_bin, http, main_guard

CONFIRM_S = 30  # kOtaConfirmDelayMs in main.cpp


def status_json():
    code, body = http("/api/status")
    try:
        return json.loads(body) if code == 200 else {}
    except ValueError:
        return {}


def run(board: Board):
    c = Checks("ota")
    board.cmd("global web_enabled 1")
    c.check("link up", board.wait_link())

    # ── upload → swap → delayed confirmation ────────────────────────────────
    before = board.get("version", "partition")
    code, body = http("/api/ota", "--data-binary", "@" + fw_bin())
    c.check("upload accepted", code == 200 and '"ok":true' in body)
    c.check("OTA image confirmed in boot log",
            board.watch_log("OTA image confirmed", deadline=CONFIRM_S + 40))
    c.check("resync", board.sync())
    good = board.get("version", "partition")
    c.check(f"partition swapped ({before} → {good})", good and good != before)
    c.check("link up after swap", board.wait_link())

    # ── rollback: reset before the confirmation ─────────────────────────────
    code, body = http("/api/ota", "--data-binary", "@" + fw_bin())
    c.check("second upload accepted", code == 200 and '"ok":true' in body)
    c.check("new image boots", board.sync())
    pending = board.get("version", "partition")
    c.check(f"booted the other slot ({pending})", pending and pending != good)
    board.cmd("reboot")  # well inside the 30 s window: never confirmed
    c.check("rollback logged at boot", board.watch_log("OTA ROLLBACK", deadline=40))
    c.check("resync after rollback", board.sync())
    c.check(f"back on the confirmed slot ({good})", board.get("version", "partition") == good)
    rb = board.get("version", "last_rollback") or ""
    c.check("console shows the rollback", "rejected" in rb and "acknowledged=0" in rb)

    c.check("link up after rollback", board.wait_link())
    c.check("/api/status carries the rollback", "rollback" in status_json())
    code, _ = http("/api/rollback/ack", "-X", "POST", "-d", "{}")
    c.check("web acknowledge is 200", code == 200)
    c.check("status no longer carries it", "rollback" not in status_json())
    c.check("console shows it acknowledged",
            "acknowledged=1" in (board.get("version", "last_rollback") or ""))

    # The rejected image must not come back on the next reboot.
    c.check("reboot (after ack)", board.reboot_and_resync())
    c.check("still on the confirmed slot", board.get("version", "partition") == good)

    board.cmd("global web_enabled 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
