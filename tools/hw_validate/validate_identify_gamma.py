#!/usr/bin/env python3
"""Identify blink, gamma/wb persistence, backup/restore round-trip."""
import json, sys, time
from pixfrog_uart import Board, Checks, http, main_guard


def run(board: Board):
    c = Checks("identify_gamma_backup")
    board.cmd("global web_enabled 1")
    board.cmd("ch 0 protocol WS2815")
    board.cmd("ch 0 universe 1")
    c.check("link up", board.wait_link())

    board.cmd("identify 0 6")
    seen = set()
    for _ in range(6):
        board.cmd("status")
        d = board.get("pixr 0 0 3", "data")
        if d:
            seen.add(d)
        time.sleep(0.3)
    c.check("identify blinks white/black", "ffffff" in seen and "000000" in seen)
    board.cmd("identify stop")
    time.sleep(0.2)
    board.cmd("dmxw 1 1 445566")
    board.cmd("status")
    c.check("decode resumes after identify", board.get("pixr 0 0 3", "data") == "445566")

    board.cmd("ch 0 gamma_x10 22")
    board.cmd("ch 0 wb ffd0a0")
    out = board.cmd("ch 0")
    c.check("gamma readback", board.kv(out, "gamma_x10") == "22")
    c.check("wb readback", board.kv(out, "wb") == "ffd0a0")

    net_before = board.get("global", "net")
    code, bk = http("/api/backup")
    if code != 200:  # first TCP after a reboot can hit a cold NAT ARP entry
        time.sleep(2)
        code, bk = http("/api/backup")
    try:
        j = json.loads(bk)
        ok = (j.get("backup_version") == 1 and len(j["channels"]) == 8
              and j["channels"][0]["gamma_x10"] == 22 and "hash" not in bk)
    except Exception:
        ok = False
    c.check("backup JSON complete (no password hash)", code == 200 and ok)
    open("/tmp/pixfrog-backup.json", "w").write(bk)

    board.cmd("ch 0 gamma_x10 30")
    board.cmd("global net 5" if net_before != "5" else "global net 6")
    code, body = http("/api/restore", "-X", "POST", "-H", "Content-Type: application/json",
                      "--data-binary", "@/tmp/pixfrog-backup.json")
    c.check("restore accepted", code == 200 and '"ok":true' in body)
    time.sleep(0.3)
    c.check("restore brings gamma back", board.get("ch 0", "gamma_x10") == "22")
    c.check("restore brings net back", board.get("global", "net") == net_before)

    board.cmd("ch 0 gamma_x10 10")
    board.cmd("ch 0 wb ffffff")
    board.cmd("global web_enabled 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
