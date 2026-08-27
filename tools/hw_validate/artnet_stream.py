#!/usr/bin/env python3
"""Stream one ArtDmx payload at a steady rate, so a logic capture taken at any
moment sees the same pixels on the wire.

Companion to nrz_decode.py: run this in the background, capture the bus, decode.
Streaming rather than a one-shot burst keeps the channel out of failsafe (which
would blank it) and survives the WSL2 NAT dropping the odd datagram.

    ./artnet_stream.py --universe 1 --hex ff0102030405060708090a0b --seconds 8
"""
import argparse
import sys
import time

from pixfrog_uart import artnet_dmx, udp_send


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--universe", type=int, default=1)
    ap.add_argument("--hex", required=True, help="DMX slot bytes, from slot 1")
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--rate", type=float, default=30.0, help="packets/sec")
    args = ap.parse_args()

    data = list(bytes.fromhex(args.hex))
    pkt_period = 1.0 / args.rate
    deadline = time.time() + args.seconds
    seq = 0
    sent = 0
    while time.time() < deadline:
        seq = seq % 255 + 1
        udp_send(artnet_dmx(args.universe, data, seq=seq), 6454)
        sent += 1
        time.sleep(pkt_period)
    print(f"sent {sent} ArtDmx packets to universe {args.universe}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
