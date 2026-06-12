#!/usr/bin/env python3
"""ArtNet end-to-end: ArtDmx → universe pool → pixel decode."""
import sys, time
from pixfrog_uart import Board, Checks, artnet_dmx, udp_send, prime_network, main_guard


def run(board: Board):
    c = Checks("artnet")
    board.cmd("ch 0 protocol WS2815")
    board.cmd("ch 0 universe 1")
    c.check("link up", board.wait_link())
    prime_network()

    before = int(board.get("stats", "artnet_packets_rx") or 0)
    udp_send(artnet_dmx(1, [0xFF, 0x01, 0x02] + [0] * 9), 6454, repeat=20)
    time.sleep(0.5)
    after = int(board.get("stats", "artnet_packets_rx") or 0)
    c.check("artnet_packets_rx increments", after > before)

    board.cmd("status")  # render tick between rx and pixel read
    c.check("pixels decoded", board.get("pixr 0 0 3", "data") == "ff0102")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
