#!/usr/bin/env python3
"""Validate ArtNet end-to-end: send UDP frames, check stats via UART.

Usage: artnet_validate.py [--port /dev/ttyACM0] [--ip 192.168.1.200]

Keeps the UART port open throughout (no board reset between steps).
"""

import argparse
import re
import socket
import struct
import sys
import threading
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*m")
TERM = re.compile(r"^(OK|ERR .*?)\r?$", re.M)


def uart_cmd(s, cmd, timeout=3):
    s.write((cmd + "\r\n").encode())
    out = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out += s.read(s.in_waiting or 1).decode(errors="replace")
        if TERM.search(ANSI.sub("", out)):
            break
    lines = [
        l.strip()
        for l in ANSI.sub("", out).splitlines()
        if l.strip()
        and not l.strip().startswith("pixfrog>")
        and l.strip() != cmd
    ]
    return "\n".join(lines)


def parse_kv(text):
    kv = {}
    for line in text.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()
    return kv


def build_artdmx(net, sub_uni, slot0_value):
    """Build a minimal ArtDmx packet with 512 channels, first channel = slot0_value."""
    pkt = bytearray(18 + 512)
    pkt[0:8] = b"Art-Net\x00"
    pkt[8] = 0x00   # OpDmx lo
    pkt[9] = 0x50   # OpDmx hi  → LE 0x5000
    pkt[10] = 0x00  # ProtoVer hi
    pkt[11] = 0x0E  # ProtoVer lo (14)
    pkt[12] = 0x01  # Sequence
    pkt[13] = 0x00  # Physical
    pkt[14] = sub_uni & 0xFF
    pkt[15] = net & 0x7F
    pkt[16] = 0x02  # DataLen hi (512)
    pkt[17] = 0x00  # DataLen lo
    pkt[18] = slot0_value & 0xFF
    for i in range(1, 512):
        pkt[18 + i] = i % 256
    return bytes(pkt)


def build_artpoll():
    pkt = bytearray(14)
    pkt[0:8] = b"Art-Net\x00"
    pkt[8] = 0x00   # OpPoll lo
    pkt[9] = 0x20   # OpPoll hi  → LE 0x2000
    pkt[10] = 0x00
    pkt[11] = 0x0E
    pkt[12] = 0x02  # TalkToMe
    pkt[13] = 0x00
    return bytes(pkt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--ip", default="192.168.1.200")
    ap.add_argument("--artnet-port", type=int, default=6454)
    args = ap.parse_args()

    print(f"[1] Opening UART {args.port} (dtr=False to avoid reset)...")
    s = serial.Serial()
    s.port = args.port
    s.baudrate = 115200
    s.timeout = 1
    s.dtr = False
    s.rts = False
    s.open()

    print("[2] Syncing on pixfrog> prompt (up to 15 s)...")
    buf = ""
    deadline = time.time() + 15
    while "pixfrog>" not in buf:
        if time.time() > deadline:
            print("ERR: no prompt — is firmware running?")
            sys.exit(1)
        s.write(b"\r\n")
        time.sleep(0.5)
        buf += s.read(s.in_waiting or 1).decode(errors="replace")
    s.reset_input_buffer()
    print("    synced!")

    # Suppress device logs so they don't pollute kv parsing
    uart_cmd(s, "loglevel none")

    print("[3] Reading board configuration...")
    g_out = uart_cmd(s, "global")
    g = parse_kv(g_out)
    artnet_net    = int(g.get("artnet_net", 0))
    artnet_subnet = int(g.get("artnet_subnet", 0))
    print(f"    artnet_net={artnet_net}  artnet_subnet={artnet_subnet}")

    ch0_out = uart_cmd(s, "ch 0")
    ch0 = parse_kv(ch0_out)
    universe_start = int(ch0.get("universe_start", 1))
    print(f"    ch0 universe_start={universe_start}")

    # The sub_uni for a given 15-bit universe:
    #   universe = (net << 8) | (subnet << 4) | port_uni
    # For the target universe_start, sub_uni = (artnet_subnet << 4) | (universe_start & 0x0F)
    target_sub_uni = ((artnet_subnet & 0x0F) << 4) | (universe_start & 0x0F)
    target_net = artnet_net & 0x7F
    print(f"    → sending sub_uni=0x{target_sub_uni:02X} net={target_net}")

    print("[4] Baseline stats...")
    stats_out = uart_cmd(s, "stats")
    stats_before = parse_kv(stats_out)
    rx_before  = int(stats_before.get("artnet_packets_rx", 0))
    bad_before = int(stats_before.get("artnet_bad_packets", 0))
    print(f"    artnet_packets_rx={rx_before}  artnet_bad_packets={bad_before}")

    status_out = uart_cmd(s, "status")
    st = parse_kv(status_out)
    print(f"    uptime_ms={st.get('uptime_ms')}  link={st.get('link')}  ip={st.get('ip')}")

    # Wait for Ethernet link if not yet up
    if st.get("link") != "1":
        print("    link=0 — waiting up to 20 s for Ethernet link-up...")
        deadline = time.time() + 20
        while time.time() < deadline:
            time.sleep(1)
            st2_out = uart_cmd(s, "status")
            st2 = parse_kv(st2_out)
            uptime = st2.get("uptime_ms", "?")
            link = st2.get("link", "0")
            print(f"    uptime_ms={uptime}  link={link}")
            if link == "1":
                print("    Ethernet link UP — continuing")
                break
        else:
            print("ERROR: Ethernet link never came up — cannot send ArtNet")
            s.close()
            sys.exit(1)

    # ── UDP send ──────────────────────────────────────────────────────────────
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_sock.bind(("", 0))

    artdmx_pkt  = build_artdmx(target_net, target_sub_uni, 0xFF)
    artpoll_pkt = build_artpoll()
    target = (args.ip, args.artnet_port)

    sent_dmx  = 0
    sent_poll = 0

    def sender():
        nonlocal sent_dmx, sent_poll
        for _ in range(20):
            udp_sock.sendto(artdmx_pkt, target)
            sent_dmx += 1
            time.sleep(0.1)
            udp_sock.sendto(artpoll_pkt, target)
            sent_poll += 1
            time.sleep(0.4)

    print(f"\n[5] Sending 20× ArtDmx + 20× ArtPoll → {args.ip}:{args.artnet_port} ...")
    t = threading.Thread(target=sender, daemon=True)
    t.start()
    t.join(timeout=15)
    print(f"    sent ArtDmx×{sent_dmx}  ArtPoll×{sent_poll}")

    # Also listen for ArtPollReply (UDP 6454)
    print("[5b] Listening 2 s for ArtPollReply...")
    reply_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    reply_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    reply_sock.settimeout(2.0)
    try:
        reply_sock.bind(("", 6454))
        try:
            data, addr = reply_sock.recvfrom(4096)
            if data[0:8] == b"Art-Net\x00":
                opcode = data[8] | (data[9] << 8)
                print(f"    RECEIVED Art-Net packet from {addr}, opcode=0x{opcode:04X}")
                if opcode == 0x2100:
                    print("    → ArtPollReply received!")
            else:
                print(f"    Received non-Art-Net packet from {addr}")
        except socket.timeout:
            print("    (no ArtPollReply received in 2 s)")
    except OSError as e:
        print(f"    (couldn't bind reply listener: {e})")
    finally:
        reply_sock.close()

    # ── Post stats ───────────────────────────────────────────────────────────
    print("\n[6] Stats after sending...")
    stats_out2 = uart_cmd(s, "stats")
    stats_after = parse_kv(stats_out2)
    rx_after  = int(stats_after.get("artnet_packets_rx", 0))
    bad_after = int(stats_after.get("artnet_bad_packets", 0))
    print(f"    artnet_packets_rx={rx_after}  (delta={rx_after - rx_before})")
    print(f"    artnet_bad_packets={bad_after} (delta={bad_after - bad_before})")

    # ── pixr check ───────────────────────────────────────────────────────────
    print("\n[7] Pixel buffer check (ch 0, first 3 pixels)...")
    pixr_out = uart_cmd(s, "pixr 0 0 3")
    print(f"    {pixr_out}")

    # ── Result ───────────────────────────────────────────────────────────────
    print("\n═══ RESULT ═══")
    delta = rx_after - rx_before
    if delta > 0:
        print(f"PASS: artnet_packets_rx increased by {delta} — UDP reception working.")
    elif (bad_after - bad_before) > 0:
        bad_delta = bad_after - bad_before
        print(f"PARTIAL: {bad_delta} bad packets received — socket receives but parse fails.")
        print("Check net/subnet config vs packet addressing.")
    else:
        print("FAIL: no packets received after sending ArtDmx+ArtPoll to 192.168.1.200:6454.")
        print("The socket is bound (no ICMP unreachable) but recvfrom() delivers nothing.")
        print("\nDiagnostic: check loglevel verbose output.")

    udp_sock.close()
    # Keep DTR=False on close to avoid reset
    s.dtr = False
    s.rts = False
    s.close()


if __name__ == "__main__":
    main()
