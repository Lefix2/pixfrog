#!/usr/bin/env python3
"""Generate minimal FSEQ v2 fixture files for the host unit tests.

Usage:
    python3 gen_fseq.py          # writes *.fseq in the current directory
"""
import struct
import zstandard as zstd
import os

OUTDIR = os.path.dirname(__file__)


def make_header(
    channel_count: int,
    frame_count: int,
    step_ms: int,
    comp_type: int,
    num_comp_blocks: int,
    num_sparse: int,
    data_offset: int,
) -> bytes:
    magic = b"PSEQ"
    minor, major = 0, 2
    header_length = 32 + num_comp_blocks * 8 + num_sparse * 6
    uuid = b"\x00" * 8
    return struct.pack(
        "<4sHBBHIIBBBBBB8s",
        magic,
        data_offset,
        minor,
        major,
        header_length,
        channel_count,
        frame_count,
        step_ms,
        0,  # flags
        comp_type,
        num_comp_blocks,
        num_sparse,
        0,  # reserved
        uuid,
    )


# ── Uncompressed, 3 universes (1536 channels), 4 frames, 40 ms/frame ──────────
def write_uncompressed():
    channel_count = 1536  # 3 universes
    frame_count = 4
    step_ms = 40
    data_offset = 32
    hdr = make_header(channel_count, frame_count, step_ms, 0, 0, 0, data_offset)
    frames = b""
    for fn in range(frame_count):
        # Each frame: universe 0 = fn*10, universe 1 = fn*20, universe 2 = fn*30
        frames += bytes([fn * 10] * 512) + bytes([fn * 20] * 512) + bytes([fn * 30] * 512)
    path = os.path.join(OUTDIR, "fixture_uncomp.fseq")
    with open(path, "wb") as f:
        f.write(hdr + frames)
    print(f"wrote {path}")


# ── zstd-compressed, 2 blocks, 4 frames ───────────────────────────────────────
def write_zstd():
    channel_count = 512  # 1 universe
    frame_count = 4
    step_ms = 25
    num_comp_blocks = 2

    cctx = zstd.ZstdCompressor(level=3)

    # Block 0: frames 0..1
    raw0 = bytes([0xAA] * 512) + bytes([0xBB] * 512)
    comp0 = cctx.compress(raw0)
    # Block 1: frames 2..3
    raw1 = bytes([0xCC] * 512) + bytes([0xDD] * 512)
    comp1 = cctx.compress(raw1)

    # Data starts right after the header + comp-block table.
    data_offset = 32 + num_comp_blocks * 8  # 48

    # Comp block table
    comp_blocks = struct.pack("<II", 0, len(comp0))  # block 0
    comp_blocks += struct.pack("<II", 2, len(comp1))  # block 1

    hdr = make_header(
        channel_count, frame_count, step_ms, 1,  # comp_type=1 (zstd)
        num_comp_blocks, 0, data_offset,
    )
    path = os.path.join(OUTDIR, "fixture_zstd.fseq")
    with open(path, "wb") as f:
        f.write(hdr + comp_blocks + comp0 + comp1)
    print(f"wrote {path}")


# ── Sparse ranges, 2 ranges, uncompressed ─────────────────────────────────────
def write_sparse():
    # Range 0: channels 0..511 (universe 1)
    # Range 1: channels 1024..1535 (universe 3, skip universe 2)
    ranges_raw = struct.pack("<IH", 0, 512) + struct.pack("<IH", 1024, 512)
    num_sparse = 2
    channel_count = 1024  # sum of range lengths
    frame_count = 2
    step_ms = 50
    data_offset = 32 + num_sparse * 6  # 44

    hdr = make_header(channel_count, frame_count, step_ms, 0, 0, num_sparse, data_offset)
    # Frame 0: range0 = 0x11, range1 = 0x22
    # Frame 1: range0 = 0x33, range1 = 0x44
    frames = bytes([0x11] * 512 + [0x22] * 512) + bytes([0x33] * 512 + [0x44] * 512)
    path = os.path.join(OUTDIR, "fixture_sparse.fseq")
    with open(path, "wb") as f:
        f.write(hdr + ranges_raw + frames)
    print(f"wrote {path}")


if __name__ == "__main__":
    write_uncompressed()
    write_zstd()
    write_sparse()
