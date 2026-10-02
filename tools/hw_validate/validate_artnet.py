#!/usr/bin/env python3
"""ArtNet end-to-end: ArtDmx → universe pool → pixel decode, and sync mode
(ArtSync publishes every universe of a frame together, free-run after 4 s)."""
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

    # ── Sync mode: channel 0 over universes 1 and 2 (200 px, 600 bytes) ────
    pixels = board.get("ch 0", "pixels") or "60"
    board.cmd("ch 0 pixels 200")
    u1 = lambda: board.get("pixr 0 0 1", "data")
    u2 = lambda: board.get("pixr 0 512 1", "data")  # universe 2's first slot
    udp_send(artnet_dmx(1, [0x10]), 6454)
    udp_send(artnet_dmx(2, [0x20]), 6454)
    udp_send(artsync(), 6454)
    time.sleep(0.3)
    c.check("sync publishes both universes", u1() == "10" and u2() == "20")
    udp_send(artnet_dmx(1, [0x11]), 6454)  # half of the next frame
    time.sleep(0.3)
    c.check("half a frame waits for its sync", u1() == "10")
    udp_send(artnet_dmx(2, [0x21]), 6454)
    udp_send(artsync(), 6454)
    time.sleep(0.3)
    c.check("the next sync publishes it whole", u1() == "11" and u2() == "21")
    time.sleep(4.5)  # no ArtSync for 4 s: free-run (Art-Net 4)
    udp_send(artnet_dmx(1, [0x12]), 6454)
    time.sleep(0.3)
    c.check("free-run 4 s after the last sync", u1() == "12")
    board.cmd(f"ch 0 pixels {pixels}")
    return c.finish()


def artsync():
    pkt = bytearray(14)
    pkt[0:8] = b"Art-Net\x00"
    pkt[8:10] = (0x5200).to_bytes(2, "little")
    pkt[11] = 14
    return bytes(pkt)


if __name__ == "__main__":
    sys.exit(main_guard(run))
