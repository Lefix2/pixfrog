#!/usr/bin/env python3
"""Web ops: GET /api/status live fields, flash coredump cycle (deliberate
crash → download → erase), mDNS announcement log.

The coredump leg panics the board on purpose (`crash confirm`) and resyncs
after the reboot. Needs the coredump partition — boards flashed with the
pre-coredump partition table fail that leg (expected until the one-time
USB reflash).
"""
import json
import os
import subprocess
import sys
import time

from pixfrog_uart import BOARD_IP, Board, Checks, http, main_guard, prime_network


def run(board: Board):
    c = Checks("webops")
    c.check("link up", board.wait_link())

    web_was_on = board.get("global", "web_enabled") == "1"
    if not web_was_on:
        board.cmd("global web_enabled 1")
        time.sleep(1)
    prime_network()

    # ── /api/status ─────────────────────────────────────────────────────────
    code, body = http("/api/status")
    c.check("GET /api/status is 200", code == 200)
    try:
        j = json.loads(body)
    except ValueError:
        j = {}
    c.check("status has heap/fps/uptime", all(k in j for k in ("heap_free", "fps", "uptime_s")))
    c.check("status has 8 channel entries", len(j.get("channels", [])) == 8)
    c.check("channel entries carry active+failsafe",
            all("active" in ch and "failsafe" in ch for ch in j.get("channels", [])))
    c.check("status has fseq block", "sd" in j.get("fseq", {}))
    c.check("heap_free plausible (> 1 MB)", j.get("heap_free", 0) > 1024 * 1024)

    # ── mDNS announcement (toggle web off/on and watch the log) ────────────
    board.cmd("global web_enabled 0")
    board.ser.reset_input_buffer()
    board.ser.write(b"global web_enabled 1\r\n")
    c.check("mDNS announced on web start", board.watch_log("mDNS: pixfrog.local", deadline=10))
    board.sync(deadline=5)
    time.sleep(1)

    # ── Coredump cycle ──────────────────────────────────────────────────────
    http("/api/coredump", "-X", "DELETE")  # clear any leftover dump
    board.ser.write(b"crash confirm\r\n")
    c.check("resync after deliberate crash", board.sync())
    c.check("link back after crash", board.wait_link())
    prime_network()

    r = subprocess.run(
        ["curl", "-s", "-m", "30", "-o", "/tmp/pixfrog-core.elf", "-w", "%{http_code}",
         f"http://{BOARD_IP}/api/coredump"],
        capture_output=True, text=True)
    c.check("GET /api/coredump is 200 after crash", r.stdout.strip() == "200")
    elf_ok = False
    if os.path.exists("/tmp/pixfrog-core.elf"):
        with open("/tmp/pixfrog-core.elf", "rb") as f:
            magic = f.read(4)
        elf_ok = magic == b"\x7fELF" and os.path.getsize("/tmp/pixfrog-core.elf") > 4096
    c.check("download is a real ELF (> 4 KB)", elf_ok)

    code, body = http("/api/coredump", "-X", "DELETE")
    c.check("DELETE /api/coredump is 200", code == 200 and '"ok":true' in body)
    code, _ = http("/api/coredump")
    c.check("GET after erase is 404", code == 404)

    # ── Restore ─────────────────────────────────────────────────────────────
    if not web_was_on:
        board.cmd("global web_enabled 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
