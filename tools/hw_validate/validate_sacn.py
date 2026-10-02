#!/usr/bin/env python3
"""sACN (unicast) end-to-end: E1.31 data → universe pool → pixel decode; the
per-source gate drops duplicates and late packets (§6.7.2) and lets a source
lower its own priority."""
import sys, time
from pixfrog_uart import Board, Checks, sacn_data, udp_send, prime_network, main_guard


def run(board: Board):
    c = Checks("sacn")
    board.cmd("global sacn_enabled 1")
    board.cmd("ch 0 protocol WS2815")
    board.cmd("ch 0 universe 1")
    c.check("link up", board.wait_link())
    prime_network()

    before = int(board.get("stats", "sacn_packets_rx") or 0)
    for i in range(20):
        udp_send(sacn_data(1, [0x11, 0x22, 0x33] + [0] * 9, seq=i), 5568)
        time.sleep(0.02)
    time.sleep(0.5)
    after = int(board.get("stats", "sacn_packets_rx") or 0)
    c.check("sacn_packets_rx increments", after > before)

    board.cmd("status")
    c.check("pixels decoded", board.get("pixr 0 0 3", "data") == "112233")

    # One step at a time, each sent 3 times (the WSL NAT drops lone
    # datagrams; the repeats are duplicates the gate must drop anyway).
    px = lambda: board.get("pixr 0 0 1", "data")

    def step(val, seq, priority=100):
        udp_send(sacn_data(1, [val], priority=priority, seq=seq), 5568, repeat=3)
        time.sleep(0.3)
        return px()

    c.check("in-order packet applied", step(0x40, 100) == "40")
    c.check("same sequence again dropped", step(0x41, 100) == "40")
    c.check("late packet dropped", step(0x42, 97) == "40")
    c.check("a source may lower its own priority", step(0x43, 101, priority=50) == "43")

    board.cmd("global sacn_enabled 0")  # restore opt-in default
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
