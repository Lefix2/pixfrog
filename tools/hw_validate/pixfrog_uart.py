#!/usr/bin/env python3
"""Shared harness for the pixfrog hardware validation suite.

Every validator keeps ONE serial session open for its whole run: opening
/dev/ttyACM0 auto-resets the board (DTR/RTS wiring of the USB-UART bridge),
so close/reopen cycles between steps would reboot it mid-test.

Environment: PORT (default /dev/ttyACM0), BOARD_IP (default 192.168.1.200).
WSL2 note: if the port vanished, re-attach from Windows:
    usbipd.exe attach --wsl --busid <BUSID>
"""

import json
import os
import subprocess
import sys
import time

import serial

PORT = os.environ.get("PORT", "/dev/ttyACM0")
BOARD_IP = os.environ.get("BOARD_IP", "192.168.1.200")


class Board:
    """Held-open UART session + the `key=value` console protocol."""

    def __init__(self, port=PORT):
        self.ser = serial.Serial(port, 115200, timeout=0.2)

    def sync(self, deadline=25):
        """Wait for the `pixfrog>` prompt (the open() reset takes ~4 s)."""
        buf, t0 = b"", time.time()
        self.ser.write(b"\r\n")
        while time.time() - t0 < deadline:
            buf += self.ser.read(256)
            if b"pixfrog>" in buf:
                return True
            if time.time() - t0 > 2:
                self.ser.write(b"\r\n")
                time.sleep(0.5)
        return False

    def cmd(self, c, deadline=5):
        """Send one console command, return everything up to OK/ERR."""
        self.ser.reset_input_buffer()
        self.ser.write(c.encode() + b"\r\n")
        buf, t0 = b"", time.time()
        while time.time() - t0 < deadline:
            buf += self.ser.read(256)
            if b"\nOK" in buf or b"ERR" in buf:
                break
        return buf.decode(errors="replace")

    def kv(self, out, key):
        """Extract `key=value` from console output (None if absent)."""
        for line in out.splitlines():
            line = line.strip()
            if line.startswith(key + "="):
                return line.split("=", 1)[1]
        return None

    def get(self, c, key, deadline=5):
        return self.kv(self.cmd(c, deadline), key)

    def wait_link(self, deadline=20):
        """Block until Ethernet link is up and an IP is assigned."""
        for _ in range(deadline):
            st = self.cmd("status")
            if self.kv(st, "link") == "1" and self.kv(st, "ip") not in (None, "0.0.0.0"):
                return True
            time.sleep(1)
        return False

    def reboot_and_resync(self):
        self.ser.write(b"reboot\r\n")
        time.sleep(1)
        return self.sync()

    def watch_log(self, needle, deadline=25):
        """Read raw output (e.g. across a self-reboot) until `needle` shows."""
        buf, t0 = b"", time.time()
        while time.time() - t0 < deadline:
            buf += self.ser.read(512)
            if needle.encode() in buf:
                return True
        return False

    def close(self):
        self.ser.close()


class Checks:
    """PASS/FAIL accounting with the suite's uniform output format."""

    def __init__(self, name):
        self.name = name
        self.results = []
        print(f"=== {name} ===")

    def check(self, label, cond):
        self.results.append((label, bool(cond)))
        print(("  PASS  " if cond else "  FAIL  ") + label)
        return cond

    def finish(self):
        failed = [n for n, ok in self.results if not ok]
        print(f"\n{len(self.results) - len(failed)}/{len(self.results)} checks passed")
        print("RESULT:", "PASS" if not failed else "FAIL: " + ", ".join(failed))
        return 0 if not failed else 1


def prime_network():
    """Resolve the NAT/host ARP entry for the board before counted UDP
    tests — the first packets to a cold entry are silently dropped."""
    subprocess.run(["ping", "-c", "1", "-W", "2", BOARD_IP], capture_output=True)
    time.sleep(0.3)


def curl(*args, timeout=120):
    """HTTP helper returning (status_code, body). Binary-safe: bodies are
    decoded with errors=replace (some endpoints return raw octets)."""
    r = subprocess.run(
        ["curl", "-s", "-m", str(timeout), "-w", "\n%{http_code}", *args],
        capture_output=True,
    )
    out = r.stdout.decode(errors="replace")
    body, _, code = out.rpartition("\n")
    return int(code or 0), body


def http(path, *args, timeout=120):
    return curl(*args, f"http://{BOARD_IP}{path}", timeout=timeout)


def fw_bin():
    """Path of the firmware image used by OTA-based validators."""
    return os.environ.get(
        "PIXFROG_BIN",
        os.path.join(os.path.dirname(__file__), "..", "..", "build", "pixfrog.bin"),
    )


def ota_flash(board, checks):
    """OTA the current build and resync; assumes web_enabled + link up."""
    code, body = http("/api/ota", "--data-binary", "@" + fw_bin())
    ok = code == 200 and '"ok":true' in body
    checks.check("OTA upload ok", ok)
    if not ok:
        return False
    time.sleep(2)
    if not board.sync():
        checks.check("resync after OTA reboot", False)
        return False
    return board.wait_link()


# ── Network packet builders (ArtNet / sACN) ─────────────────────────────────

def artnet_dmx(universe, data, artnet_net=0, artnet_subnet=0, seq=0):
    pkt = bytearray(18 + len(data))
    pkt[0:8] = b"Art-Net\x00"
    pkt[8:10] = (0x5000).to_bytes(2, "little")
    pkt[11] = 14
    pkt[12] = seq
    pkt[14] = ((artnet_subnet & 0xF) << 4) | (universe & 0xF)
    pkt[15] = (universe >> 8) & 0x7F if artnet_net == 0 else artnet_net & 0x7F
    pkt[16] = len(data) >> 8
    pkt[17] = len(data) & 0xFF
    pkt[18:] = bytes(data)
    return bytes(pkt)


def artnet_trigger(subkey, key=3):
    pkt = bytearray(18)
    pkt[0:8] = b"Art-Net\x00"
    pkt[8:10] = (0x9900).to_bytes(2, "little")
    pkt[11] = 14
    pkt[14], pkt[15] = 0xFF, 0xFF  # Oem global
    pkt[16], pkt[17] = key, subkey
    return bytes(pkt)


def sacn_data(universe, slots, priority=100, seq=0):
    import struct

    p = bytearray(126 + len(slots))
    struct.pack_into(">HH", p, 0, 0x0010, 0x0000)
    p[4:16] = b"ASC-E1.17\x00\x00\x00"
    struct.pack_into(">H", p, 16, 0x7000 | ((len(p) - 16) & 0xFFF))
    struct.pack_into(">I", p, 18, 0x00000004)
    p[22:38] = bytes(range(16))
    struct.pack_into(">H", p, 38, 0x7000 | ((len(p) - 38) & 0xFFF))
    struct.pack_into(">I", p, 40, 0x00000002)
    p[44:54] = b"hw-validate"[:10]
    p[108] = priority
    p[111] = seq
    struct.pack_into(">H", p, 113, universe)
    struct.pack_into(">H", p, 115, 0x7000 | ((len(p) - 115) & 0xFFF))
    p[117], p[118] = 0x02, 0xA1
    struct.pack_into(">H", p, 121, 1)
    struct.pack_into(">H", p, 123, 1 + len(slots))
    p[126:] = bytes(slots)
    return bytes(p)


def udp_send(pkt, port, host=BOARD_IP, repeat=1, gap=0.05):
    """Repeats spread the burst past NAT/ARP latency (single datagrams from
    a WSL2 NAT routinely vanish on a cold ARP entry)."""
    import socket

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for _ in range(repeat):
        s.sendto(pkt, (host, port))
        if repeat > 1:
            time.sleep(gap)
    s.close()


def main_guard(run):
    """Standard entry: open board, sync, run, return exit code."""
    board = Board()
    if not board.sync():
        print("SYNC FAILED (board prompt not seen)")
        return 1
    try:
        return run(board)
    finally:
        board.close()


if __name__ == "__main__":
    # Smoke: open the port, sync, print version.
    def run(board):
        print(board.cmd("version"))
        return 0

    sys.exit(main_guard(run))
