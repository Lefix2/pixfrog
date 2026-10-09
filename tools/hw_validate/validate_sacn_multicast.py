#!/usr/bin/env python3
"""sACN multicast on a real LAN: E1.31 to the 239.255.x.y groups (not unicast)
reaches the pixels; a universe re-config moves the join (5 s refresh) and the
old group is dropped; the control universe works on its group.

Needs a sender on the board's LAN: MCAST_FROM = its IPv4 address. Under WSL
(NAT: multicast stays in) the packets leave from Windows via mcast_send.ps1.
Not in run_all's default list."""
import os, socket, struct, subprocess, sys, time
from pixfrog_uart import Board, Checks, sacn_data, main_guard

HERE = os.path.dirname(os.path.abspath(__file__))
FROM = os.environ.get("MCAST_FROM", "")
WSL = "microsoft" in open("/proc/version").read().lower() if os.path.exists("/proc/version") else False
_seq = [0]


def mcast(uni, slots, count=20):
    pkt = bytearray(sacn_data(uni, slots, seq=_seq[0]))
    _seq[0] = (_seq[0] + count) % 256
    group = f"239.255.{uni >> 8}.{uni & 0xFF}"
    if WSL:
        ps1 = subprocess.run(["wslpath", "-w", os.path.join(HERE, "mcast_send.ps1")],
                             capture_output=True, text=True).stdout.strip()
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1,
                        "-From", FROM, "-Group", group, "-Hex", pkt.hex(), "-Count", str(count)],
                       check=True)
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(FROM))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
        for k in range(count):
            pkt[111] = (pkt[111] + 1) % 256
            s.sendto(bytes(pkt), (group, 5568))
            time.sleep(0.025)
        s.close()
    time.sleep(0.3)


def run(board: Board):
    c = Checks("sacn_multicast")
    if not FROM:
        c.check("MCAST_FROM set (the sender's IPv4 on the board's LAN)", False)
        return c.finish()
    rx = lambda: int(board.get("stats", "sacn_packets_rx") or 0)
    px = lambda: (board.cmd("status"), board.get("pixr 0 0 3", "data"))[1]
    was = {k: board.get("ch 0", k) for k in ("universe", "protocol", "fixture_ctl")}
    sacn_was = board.get("global", "sacn_enabled")
    try:
        board.cmd("ch 0 fixture_ctl 0")  # pixels alone: a fixture dimmer elsewhere would black them
        board.cmd("ch 0 protocol WS2815")
        board.cmd("ch 0 universe 7")
        board.cmd("global sacn_enabled 1")
        c.check("link up", board.wait_link())
        time.sleep(6)  # the join refresh (5 s)

        r = rx(); mcast(7, [0x11, 0x22, 0x33])
        c.check(f"239.255.0.7 received (+{rx() - r})", rx() > r)
        c.check("pixels decoded from multicast", px() == "112233")

        board.cmd("ch 0 universe 9"); time.sleep(6.5)
        mcast(9, [0x44, 0x55, 0x66])
        c.check("re-config: 239.255.0.9 drives the pixels", px() == "445566")
        r = rx(); mcast(7, [0xAA, 0xAA, 0xAA])
        c.check("old group 239.255.0.7 no longer routed", rx() == r and px() == "445566")

        board.cmd("ctrl preset simple"); board.cmd("ctrl universe 20")
        board.cmd("ctrl address 1"); board.cmd("ctrl enable 1")
        time.sleep(6.5)
        r = rx(); mcast(20, [0x80, 0x00, 0, 0, 0, 0])
        c.check(f"control group 239.255.0.20 received (+{rx() - r})", rx() > r)
        c.check("control universe live", board.get("show", "control") == "live")
        m = (board.get("show", "master") or "").split(",")[0]
        c.check(f"desk master 50 % ({m})", m.isdigit() and abs(int(m) - 50) <= 1)
    finally:
        board.cmd("ctrl enable 0")
        for k, v in was.items():
            board.cmd(f"ch 0 {k} {v}")
        board.cmd(f"global sacn_enabled {sacn_was}")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
