#!/usr/bin/env python3
"""Decode a captured LED bus data line back into pixel bytes.

Closes the last hop of the end-to-end chain. validate_artnet.py proves
ArtDmx -> universe pool -> pixel buffer by reading the board back over UART;
this decodes what actually left the GPIO, which is the only check that can
catch a fault in the encode/DMA path. Both #74 and its telemetry looked
perfectly healthy while the bus sat silent.

Input is a Logic 2 "Export Raw Data -> CSV" digital export (one row per
transition, `Time [s],Channel 0,...`). Sample the bus at >= 20x PCLK; 50 MS/s
against the 16 MHz PCLK gives ~3 samples per PCLK tick, plenty to separate a
T0H from a T1H.

    ./nrz_decode.py digital.csv --channel 0 --protocol ws2815 --pixels 4

Bus bit k is GPIO kLedBusGpio[k] (boards/esp32_p4_devkit.h); pixfrog channel n
drives data on bus bit 2n and clock on 2n+1, so an NRZ channel only ever moves
its even bit. --channel here is the *Saleae* column, not the pixfrog channel:
which probe sits on which bit is wiring, so confirm it with `cal 1` (walking-1
across the 16 bits) rather than assuming.
"""
import argparse
import csv
import sys

PCLK_HZ = 16_000_000  # led_protocols::kPclkHz

# samples_t0h, samples_t1h, samples_bit, samples_reset — led_protocols.cpp
# timing_for(). Values are in PCLK ticks at 16 MHz.
PROTOCOLS = {
    "ws2815": (5, 15, 20, 4480),
    "ws2814": (5, 15, 20, 4480),
    "ws2812b": (6, 11, 20, 800),
    "ws2811": (6, 11, 20, 800),
    "sk6812": (5, 10, 19, 1280),
}


def read_edges(path, column):
    """(time, level) transitions for one digital column, in seconds."""
    edges = []
    with open(path, newline="") as fh:
        rows = csv.reader(fh)
        header = next(rows)
        if column + 1 >= len(header):
            raise SystemExit(f"column {column} not in export (header: {header})")
        prev = None
        for row in rows:
            level = int(row[column + 1])
            if prev is not None and level != prev:
                edges.append((float(row[0]), level))
            prev = level
    return edges


def decode(edges, protocol, reset_frac=0.5):
    """Split on reset gaps and decode each frame's HIGH pulses into bytes.

    A bit is 1 when its HIGH pulse is nearer T1H than T0H. Frames are separated
    by any LOW longer than `reset_frac` of the protocol's reset tail — well
    clear of the ~1 PCLK-tick LOW inside a bit cell, so the split never lands
    mid-frame even if the capture starts partway through one.
    """
    t0h, t1h, _bit, reset = PROTOCOLS[protocol]
    tick = 1.0 / PCLK_HZ
    mid = (t0h + t1h) / 2.0 * tick
    gap_min = reset * reset_frac * tick

    frames, bits = [], []
    for i in range(len(edges) - 1):
        t, level = edges[i]
        width = edges[i + 1][0] - t
        if level == 1:
            bits.append(1 if width > mid else 0)
        elif width > gap_min:
            if bits:
                frames.append(bits)
            bits = []
    if bits:
        frames.append(bits)

    out = []
    for f in frames:
        data = bytearray()
        for i in range(0, len(f) - 7, 8):  # MSB first, drop a partial tail byte
            byte = 0
            for b in f[i : i + 8]:
                byte = (byte << 1) | b
            data.append(byte)
        out.append((bytes(data), len(f)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--channel", type=int, default=0, help="Saleae digital column")
    ap.add_argument("--protocol", default="ws2815", choices=sorted(PROTOCOLS))
    ap.add_argument("--pixels", type=int, default=0, help="expected pixel count (0 = any)")
    ap.add_argument("--bytes-per-pixel", type=int, default=3)
    ap.add_argument("--expect", help="expected hex for the whole frame, e.g. ff0102...")
    ap.add_argument("--frames", type=int, default=3, help="how many frames to print")
    args = ap.parse_args()

    edges = read_edges(args.csv, args.channel)
    if not edges:
        print(f"FAIL: no transitions on channel {args.channel} — bus silent")
        return 1
    frames = decode(edges, args.protocol)
    if not frames:
        print(f"FAIL: {len(edges)} edges but no complete frame")
        return 1

    want_bits = args.pixels * args.bytes_per_pixel * 8 if args.pixels else None
    # The capture almost never starts on a frame boundary, so the first and last
    # frames are usually partial. Judge the whole-frame ones.
    whole = [f for f in frames if want_bits is None or f[1] == want_bits]
    print(f"channel {args.channel}: {len(edges)} edges, {len(frames)} frames, "
          f"{len(whole)} complete")
    for data, nbits in frames[: args.frames]:
        print(f"  {nbits:5d} bits  {data.hex()}")

    if want_bits is not None and not whole:
        got = sorted({f[1] for f in frames})
        print(f"FAIL: no frame of {want_bits} bits (saw {got})")
        return 1
    payloads = {f[0] for f in whole}
    if len(payloads) > 1:
        print(f"FAIL: frames disagree — {len(payloads)} distinct payloads")
        return 1
    if args.expect:
        want = bytes.fromhex(args.expect)
        got = whole[0][0]
        if got != want:
            print(f"FAIL: expected {want.hex()}, got {got.hex()}")
            return 1
        print(f"PASS: {len(whole)} frames all match {want.hex()}")
    else:
        print(f"PASS: {len(whole)} identical frames, payload {whole[0][0].hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
