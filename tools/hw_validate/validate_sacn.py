#!/usr/bin/env python3
"""sACN (unicast) end-to-end: E1.31 data → universe pool → pixel decode."""
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

    board.cmd("global sacn_enabled 0")  # restore opt-in default
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
