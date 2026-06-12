#!/usr/bin/env python3
"""FSEQ networking: console seek, ArtTimeCode slave, web upload, FPP MultiSync.

Needs a microSD card in the slot. The validator uploads its own tiny test
sequence (hwtest.fseq, 20 s of black @ 25 fps) over HTTP, so the card does
not need any pre-existing file. web_enabled is turned on for the upload and
restored afterwards.
"""
import struct
import sys
import time

from pixfrog_uart import (BOARD_IP, Board, Checks, http, main_guard,
                          prime_network, udp_send)

TEST_FILE = "hwtest.fseq"


def make_fseq(frames=800, channels=12, step_ms=25):
    """Minimal valid FSEQ v2, uncompressed, no sparse ranges."""
    hdr = struct.pack(
        "<4sHBBHIIBBBBBB8s",
        b"PSEQ",
        32,        # channel_data_offset
        0, 2,      # minor, major version
        32,        # header_length
        channels,
        frames,
        step_ms,
        0,         # flags
        0,         # compression: none
        0, 0, 0,   # comp blocks, sparse ranges, reserved
        b"\x00" * 8,
    )
    return hdr + bytes(channels) * frames


def artnet_timecode(frames, seconds, minutes, hours, tc_type=1):
    pkt = bytearray(19)
    pkt[0:8] = b"Art-Net\x00"
    pkt[8:10] = (0x9700).to_bytes(2, "little")
    pkt[11] = 14
    pkt[14], pkt[15], pkt[16], pkt[17], pkt[18] = frames, seconds, minutes, hours, tc_type
    return bytes(pkt)


def fpp_sync(action, seconds=0.0, frame=0, filename=TEST_FILE):
    """FPP MultiSync packet: FPPD header + sync payload."""
    name = filename.encode() + b"\x00"
    pkt = b"FPPD" + bytes([1]) + struct.pack("<H", 10 + len(name))
    pkt += bytes([action, 0]) + struct.pack("<If", frame, seconds) + name
    return pkt


def position(board):
    return int(board.get("fseq", "position_ms") or 0)


def run(board: Board):
    c = Checks("fseq")
    c.check("link up", board.wait_link())
    prime_network()

    sd_files = board.cmd("fseq")
    if "status=" not in sd_files:
        c.check("fseq console command answers", False)
        return c.finish()

    # ── Upload the test sequence over HTTP ──────────────────────────────────
    web_was_on = board.get("global", "web_enabled") == "1"
    if not web_was_on:
        board.cmd("global web_enabled 1")
        time.sleep(1)
    with open("/tmp/hwtest.fseq", "wb") as f:
        f.write(make_fseq())
    code, body = http(f"/api/fseq/upload?name={TEST_FILE}", "--data-binary",
                      "@/tmp/hwtest.fseq")
    c.check("upload accepted (HTTP 200)", code == 200 and '"ok":true' in body)
    listed = board.cmd("fseq list")
    c.check("uploaded file listed on SD", TEST_FILE in listed)

    code, _ = http("/api/fseq/upload?name=evil.txt", "--data-binary", "@/tmp/hwtest.fseq")
    c.check("non-.fseq name rejected (HTTP 400)", code == 400)

    # ── Playback + position/duration ────────────────────────────────────────
    board.cmd(f"fseq play {TEST_FILE}")
    time.sleep(1.5)
    p1 = position(board)
    time.sleep(1.5)
    p2 = position(board)
    c.check("position advances during playback", p2 > p1 > 0)
    dur = int(board.get("fseq", "duration_ms") or 0)
    c.check("duration is 20 s (800 frames @ 25 ms)", dur == 20000)

    # ── Console seek ────────────────────────────────────────────────────────
    board.cmd("fseq seek 15000")
    time.sleep(0.3)
    p = position(board)
    c.check("console seek lands at ~15 s", 14900 <= p <= 16000)

    # ── ArtTimeCode slave ───────────────────────────────────────────────────
    udp_send(artnet_timecode(0, 5, 0, 0), 6454, repeat=3)  # 00:00:05:00 EBU
    time.sleep(0.5)
    p = position(board)
    c.check("ArtTimeCode seeks to ~5 s", 4900 <= p <= 6000)

    udp_send(artnet_timecode(13, 5, 0, 0), 6454, repeat=3)  # +520 ms: inside tolerance
    time.sleep(0.5)
    drift_ref = position(board)
    udp_send(artnet_timecode(0, 5, 0, 0), 6454, repeat=1)
    time.sleep(0.3)
    c.check("small drift does not re-seek (tolerance)", position(board) >= drift_ref - 200)

    # ── FPP MultiSync ───────────────────────────────────────────────────────
    board.cmd("fseq stop")
    udp_send(fpp_sync(action=0), 32320, repeat=3)  # start while fpp_remote=0
    time.sleep(0.5)
    c.check("FPP ignored while disabled", board.get("fseq", "status") == "idle")

    board.cmd("global fpp_remote 1")
    time.sleep(0.5)
    udp_send(fpp_sync(action=0), 32320, repeat=3)  # START
    time.sleep(0.8)
    c.check("FPP start plays the file", board.get("fseq", "active") == TEST_FILE)

    udp_send(fpp_sync(action=2, seconds=12.0, frame=480), 32320, repeat=3)  # SYNC
    time.sleep(0.5)
    p = position(board)
    c.check("FPP sync corrects to ~12 s", 11900 <= p <= 13000)

    udp_send(fpp_sync(action=1), 32320, repeat=3)  # STOP
    time.sleep(0.5)
    c.check("FPP stop ends playback", board.get("fseq", "status") == "idle")

    udp_send(fpp_sync(action=2, seconds=8.0, frame=320), 32320, repeat=3)  # hot-join
    time.sleep(0.8)
    c.check("FPP sync hot-joins a running show", board.get("fseq", "active") == TEST_FILE)
    p = position(board)
    c.check("hot-join position is ~8 s", 7800 <= p <= 9500)

    # ── Restore ─────────────────────────────────────────────────────────────
    board.cmd("fseq stop")
    board.cmd("global fpp_remote 0")
    if not web_was_on:
        board.cmd("global web_enabled 0")
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
