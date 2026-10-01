#!/usr/bin/env python3
"""Web ops: GET /api/status live fields, flash coredump cycle (deliberate
crash → download → erase), mDNS announcement log.

The coredump leg panics the board on purpose (`crash confirm`) and resyncs
after the reboot. Needs the coredump partition — boards flashed with the
pre-coredump partition table fail that leg (expected until the one-time
USB reflash).
"""
import gzip
import json
import os
import shutil
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

    # ── SPA: gzipped, cached by ETag ────────────────────────────────────────
    def head(*extra):
        r = subprocess.run(["curl", "-s", "-m", "20", "-o", "/dev/null", "-D", "-",
                            "-H", "Accept-Encoding: gzip", *extra, f"http://{BOARD_IP}/"],
                           capture_output=True, text=True)
        lines = r.stdout.splitlines()
        hdrs = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip()
                for l in lines[1:] if ":" in l}
        return (lines[0].split()[1] if lines else ""), hdrs

    status, hdrs = head()
    c.check("GET / is 200", status == "200")
    c.check("SPA served gzip", hdrs.get("content-encoding") == "gzip")
    r = subprocess.run(["curl", "-s", "-m", "20", "-H", "Accept-Encoding: gzip",
                        f"http://{BOARD_IP}/"], capture_output=True)
    try:
        page = gzip.decompress(r.stdout)
    except OSError:
        page = b""
    c.check("gzip body inflates to the SPA", page.lstrip()[:15].lower() == b"<!doctype html>")
    c.check(f"compression pays (wire {len(r.stdout)} B, page {len(page)} B)",
            0 < len(r.stdout) * 2 < len(page))
    etag = hdrs.get("etag", "")
    c.check("SPA has an ETag", len(etag) > 2)
    status, _ = head("-H", f"If-None-Match: {etag}")
    c.check("matching If-None-Match → 304", status == "304")
    status, _ = head("-H", 'If-None-Match: "stale"')
    c.check("stale If-None-Match → 200", status == "200")

    # ── mDNS announcement (toggle web off/on and watch the log) ────────────
    board.cmd("global web_enabled 0")
    board.ser.reset_input_buffer()
    board.ser.write(b"global web_enabled 1\r\n")
    c.check("mDNS announced on web start", board.watch_log("mDNS: pixfrog-", deadline=10))
    c.check("alone, it takes pixfrog.local",
            board.watch_log("pixfrog.local now points here", deadline=10))
    board.sync(deadline=5)
    time.sleep(1)
    st = json.loads(http("/api/status")[1] or "{}")
    c.check("status names the box pixfrog-xxxx",
            str(st.get("host", "")).startswith("pixfrog-") and len(st.get("host", "")) == 12)
    c.check("status says it holds the alias", st.get("alias") is True)
    # Ask the board's responder for both names (WSL has no route for mDNS
    # multicast; Windows does, through powershell.exe).
    if shutil.which("powershell.exe"):
        ps1 = subprocess.run(["wslpath", "-w", os.path.join(os.path.dirname(__file__),
                                                            "mdns_query.ps1")],
                             capture_output=True, text=True).stdout.strip()
        for name in (st.get("host", "") + ".local", "pixfrog.local"):
            out = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                  "-File", ps1, "-Name", name, "-Board", BOARD_IP],
                                 capture_output=True, text=True, timeout=30).stdout
            c.check(f"mDNS answers {name}", f"ip={BOARD_IP}" in out)

    # ── Coredump cycle ──────────────────────────────────────────────────────
    http("/api/coredump", "-X", "DELETE")  # clear any leftover dump
    board.ser.write(b"crash confirm\r\n")
    c.check("resync after deliberate crash", board.sync())
    c.check("link back after crash", board.wait_link())
    prime_network()

    code = ""
    for _ in range(3):  # first HTTP hit after the panic-reboot can be flaky
        r = subprocess.run(
            ["curl", "-s", "-m", "30", "-o", "/tmp/pixfrog-core.bin", "-w", "%{http_code}",
             f"http://{BOARD_IP}/api/coredump"],
            capture_output=True, text=True)
        code = r.stdout.strip()
        if code == "200":
            break
        time.sleep(2)
    c.check("GET /api/coredump is 200 after crash", code == "200")
    # Raw image = 24-byte esp_core_dump header, then the ELF.
    dump_ok = False
    if os.path.exists("/tmp/pixfrog-core.bin"):
        with open("/tmp/pixfrog-core.bin", "rb") as f:
            head = f.read(64)
        dump_ok = b"\x7fELF" in head and os.path.getsize("/tmp/pixfrog-core.bin") > 4096
    c.check("download is a core image with embedded ELF (> 4 KB)", dump_ok)

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
