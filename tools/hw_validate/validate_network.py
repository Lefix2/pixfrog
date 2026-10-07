#!/usr/bin/env python3
"""The addressing applied without a reboot: a static address set from the
console answers HTTP at once, DHCP brings a lease the box advertises, and the
box comes back to the address it had. Needs a DHCP server on the bench LAN
(the usual router); the box ends on the addressing it started with."""
import sys
import time

from pixfrog_uart import Board, Checks, curl, main_guard


def status_at(ip, timeout=3):
    code, body = curl(f"http://{ip}/api/status", timeout=timeout)
    return code == 200 and '"ip":"%s"' % ip in body


def wait_http(ip, deadline=20):
    end = time.time() + deadline
    while time.time() < end:
        if status_at(ip):
            return True
        time.sleep(1)
    return False


def board_ip(board):
    return board.kv(board.cmd("status"), "ip")


def run(board: Board):
    c = Checks("network")
    g = board.cmd("global")
    was_dhcp, ip0 = board.kv(g, "dhcp") == "1", board.kv(g, "ip")
    mask0, gw0 = board.kv(g, "mask"), board.kv(g, "gw")
    c.check("link up", board.wait_link())
    live0 = board_ip(board)
    c.check(f"the box answers where it says ({live0})", live0 and wait_http(live0, 5))
    # A neighbour of the current address, on the same subnet.
    a, b, cc, d = (int(x) for x in live0.split("."))
    alt = f"{a}.{b}.{cc}.{(d % 250) + 2}"

    out = board.cmd(f"global ip {alt}")
    c.check("the console says the setting was applied", board.kv(out, "note") == "network_applied")
    if was_dhcp:
        board.cmd("global dhcp 0")
    c.check(f"static {alt} answers HTTP without a reboot", wait_http(alt, 10))
    c.check("the console shows the new address", board_ip(board) == alt)
    c.check("the old address is gone", not status_at(live0, 2))

    board.cmd("global dhcp 1")
    end = time.time() + 30
    leased = ""
    while time.time() < end:
        leased = board_ip(board)
        if leased not in ("", "0.0.0.0", alt):
            break
        time.sleep(1)
    c.check(f"DHCP: a lease without a reboot ({leased})", leased not in ("", "0.0.0.0", alt))
    c.check("the leased address answers HTTP", leased and wait_http(leased, 10))

    # Back where it started.
    if was_dhcp:
        board.cmd(f"global ip {ip0}")
        board.cmd(f"global mask {mask0}")
        board.cmd(f"global gw {gw0}")
    else:
        board.cmd("global dhcp 0")
        board.cmd(f"global ip {ip0}")
    c.check(f"back on its own addressing ({live0})", wait_http(live0, 20))
    g = board.cmd("global")
    c.check("the stored addressing is what it was",
            (board.kv(g, "dhcp") == "1") == was_dhcp and board.kv(g, "ip") == ip0)
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
