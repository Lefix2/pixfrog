#!/usr/bin/env python3
"""Backlight dimming: console range checks, NVS persistence, web round-trip.

Brightness itself is not observable over UART — point a phone camera at the
panel while this runs if you want the visual confirmation. What is checked here
is every path that writes the three fields, and that they survive a reboot.
"""
import json, sys, time
from pixfrog_uart import Board, Checks, http, main_guard


def run(board: Board):
    c = Checks("display_backlight")
    before_bright = board.get("global", "tft_brightness")
    before_dim = board.get("global", "tft_idle_dim")
    before_delay = board.get("global", "tft_dim_delay_s")

    board.cmd("global tft_brightness 40")
    c.check("brightness readback", board.get("global", "tft_brightness") == "40")
    board.cmd("global tft_idle_dim 80")
    c.check("idle dim readback", board.get("global", "tft_idle_dim") == "80")

    board.cmd("global tft_dim_delay_s 45")
    c.check("dim delay readback", board.get("global", "tft_dim_delay_s") == "45")

    c.check("brightness floor enforced", "ERR" in board.cmd("global tft_brightness 5"))
    c.check("brightness ceiling enforced", "ERR" in board.cmd("global tft_brightness 101"))
    c.check("idle dim ceiling enforced", "ERR" in board.cmd("global tft_idle_dim 150"))
    c.check("dim delay ceiling enforced", "ERR" in board.cmd("global tft_dim_delay_s 3601"))
    c.check("rejects leave the level alone", board.get("global", "tft_brightness") == "40")

    c.check("reboot", board.reboot_and_resync())
    c.check("brightness persisted", board.get("global", "tft_brightness") == "40")
    c.check("idle dim persisted", board.get("global", "tft_idle_dim") == "80")
    c.check("dim delay persisted", board.get("global", "tft_dim_delay_s") == "45")

    board.cmd("global web_enabled 1")
    c.check("link up", board.wait_link())
    body = json.dumps({"tft_brightness": 65, "tft_idle_dim": 0, "tft_dim_delay_s": 120})
    code, resp = http("/api/global", "-X", "POST", "-H", "Content-Type: application/json",
                      "--data-binary", body)
    if code != 200:  # first TCP after a reboot can hit a cold NAT ARP entry
        time.sleep(2)
        code, resp = http("/api/global", "-X", "POST", "-H", "Content-Type: application/json",
                          "--data-binary", body)
    c.check("web POST accepted", code == 200)
    time.sleep(0.3)
    c.check("web sets brightness", board.get("global", "tft_brightness") == "65")
    c.check("web disables idle dim", board.get("global", "tft_idle_dim") == "0")
    c.check("web sets dim delay", board.get("global", "tft_dim_delay_s") == "120")

    code, g = http("/api/global")
    c.check("web GET exposes the three fields",
            code == 200 and '"tft_brightness":65' in g and '"tft_idle_dim":0' in g
            and '"tft_dim_delay_s":120' in g)

    board.cmd(f"global tft_brightness {before_bright}")
    board.cmd(f"global tft_idle_dim {before_dim}")
    board.cmd(f"global tft_dim_delay_s {before_delay}")
    board.cmd("global web_enabled 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
