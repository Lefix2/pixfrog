#!/usr/bin/env python3
"""Regenerates the seed corpora in tests/fuzz/corpus/<target>/ from valid
packets and requests, so the fuzzers start past every magic/opcode check.

    python3 tests/fuzz/make_seeds.py [path/to/pixfrog_api_host]

The API host (built by the tests project) supplies a real /api/backup and
/api/config document for the web seeds; without it those two are skipped.
"""
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SEP = b"\xff\xfe\xfd"  # fuzz_receivers: datagram separator


def art(op, size):
    p = bytearray(size)
    p[0:8] = b"Art-Net\0"
    p[8:10] = struct.pack("<H", op)
    p[11] = 14
    return p


def art_dmx(uni, data, seq=1):
    p = art(0x5000, 18 + len(data))
    p[12] = seq
    p[14:16] = struct.pack("<H", uni)
    p[16:18] = struct.pack(">H", len(data))
    p[18:] = data
    return bytes(p)


def art_nzs(uni, sc, data):
    p = bytearray(art_dmx(uni, data))
    p[8:10] = struct.pack("<H", 0x5100)
    p[13] = sc
    return bytes(p)


def art_address():
    p = art(0x6000, 107)
    p[14:32] = b"stage-left".ljust(18, b"\0")
    p[32:96] = b"PixFrog stage".ljust(64, b"\0")
    p[96:100] = bytes([0x81, 0x82, 0x83, 0x84])
    p[100] = 0x80
    p[106] = 0x04
    return bytes(p)


def art_ipprog(cmd=0x84):
    p = art(0xF800, 34)
    p[14] = cmd
    p[16:20] = bytes([10, 0, 0, 9])
    p[20:24] = bytes([255, 0, 0, 0])
    return bytes(p)


def art_timecode():
    p = art(0x9700, 19)
    p[14:19] = bytes([12, 30, 15, 1, 1])
    return bytes(p)


def art_trigger():
    p = art(0x9900, 530)
    p[14:16] = b"\xff\xff"
    p[16] = 1
    return bytes(p)


def sacn_data(uni, slots, prio=100, opts=0, sync=0, cid=0, sc=0):
    p = bytearray(126 + len(slots))
    p[1] = 0x10
    p[4:16] = b"ASC-E1.17\0\0\0"
    p[16:18] = struct.pack(">H", 0x7000 | (len(p) - 16))
    p[21] = 0x04
    p[22:38] = bytes((i + cid) & 0xFF for i in range(16))
    p[38:40] = struct.pack(">H", 0x7000 | (len(p) - 38))
    p[43] = 0x02
    p[44:108] = b"console".ljust(64, b"\0")
    p[108] = prio
    p[109:111] = struct.pack(">H", sync)
    p[111] = 1
    p[112] = opts
    p[113:115] = struct.pack(">H", uni)
    p[115:117] = struct.pack(">H", 0x7000 | (len(p) - 115))
    p[117] = 0x02
    p[118] = 0xA1
    p[122] = 0x01
    p[123:125] = struct.pack(">H", 1 + len(slots))
    p[125] = sc
    p[126:] = slots
    return bytes(p)


def sacn_sync(addr):
    p = bytearray(49)
    p[1] = 0x10
    p[4:16] = b"ASC-E1.17\0\0\0"
    p[21] = 0x08
    p[43] = 0x01
    p[45:47] = struct.pack(">H", addr)
    return bytes(p)


def fpp_sync(action, frame, secs, name):
    body = struct.pack("<BBIf", action, 0, frame, secs) + name + b"\0"
    return b"FPPD" + bytes([1]) + struct.pack("<H", len(body)) + body


def fseq(ranges=(), comp=0, blocks=0):
    table = 32 + 8 * blocks
    off = table + 6 * len(ranges)
    h = b"PSEQ" + struct.pack("<HBBHIIBBBBBB", off, 0, 2, 32, 1536, 100, 25, 0, comp, blocks,
                              len(ranges), 0) + bytes(8)
    h += b"".join(struct.pack("<II", i * 10, 900) for i in range(blocks))
    h += b"".join(struct.pack("<IH", s, l) for s, l in ranges)
    return h + bytes(64)


PARSERS = [
    art_dmx(1, bytes(range(96))), art_dmx(0x7FFF, bytes(512)), art_nzs(2, 0xDD, b"\1\2\3"),
    bytes(art(0x2000, 14)), bytes(art(0x5200, 14)), art_address(), art_ipprog(),
    art_ipprog(0x00), art_timecode(), art_trigger(),
    sacn_data(1, bytes(range(170))), sacn_data(63999, bytes(512), opts=0x40),
    sacn_data(3, b"\5" * 24, sync=7), sacn_sync(7),
    fpp_sync(0, 0, 0.0, b"show.fseq"), fpp_sync(2, 1234, 30.85, b"show.fseq"),
    fpp_sync(1, 0, 0.0, b"x" * 80),
    fseq(), fseq([(0, 510), (1024, 96)]), fseq(comp=1, blocks=3),
]

RECEIVERS = [
    b"\0" + art_dmx(1, bytes(range(200))),
    b"\0" + art_dmx(1, b"\x10" * 512) + SEP + art_dmx(2, b"\x20" * 88) + SEP + bytes(art(0x5200, 14)),
    b"\0" + art_dmx(1, b"\x10" * 30, 1) + SEP + art_dmx(1, b"\x90" * 30, 2),
    b"\2" + art_dmx(1, b"\x40" * 60) + SEP + art_dmx(1, b"\x01" * 60),  # other source: merge
    b"\0" + bytes(art(0x2000, 14)) + SEP + art_address() + SEP + art_ipprog(),
    b"\0" + art_nzs(1, 0xDD, b"\7" * 10) + SEP + art_timecode() + SEP + art_trigger(),
    b"\1" + sacn_data(1, bytes(range(170))),
    b"\1" + sacn_data(1, b"\x11" * 510, prio=100) + SEP + sacn_data(1, b"\x22" * 510, prio=150, cid=9),
    b"\1" + sacn_data(1, b"\5" * 60, sync=7) + SEP + sacn_data(2, b"\6" * 60, sync=7) + SEP +
    sacn_sync(7),
    b"\1" + sacn_data(1, b"\1" * 9, opts=0x40),  # stream terminated
    b"\1" + sacn_data(4, b"\3" * 30, sc=0xDD),
]

CONSOLE = """status
version
stats
chstat
ch 0
ch 0 protocol WS2815
ch 0 pixels 300
ch 0 universe 1
ch 2 order GRB
ch 2 clock_hz 100
ch 2 wb ff8000
ch 2 gaps 1:1,50:2
ch 2 gaps -
global
global refresh_hz 45
global failsafe_color 112233
global ip 10.0.0.9
global short_name stage-left
global web_password hunter2
global sacn_enabled 1
global fpp_remote 1
dmxw 1 1 aabbcc
dmxr 1 1 3
pixr 0 0 3
identify 3 5
autopatch 0
cal 1
scene
scene add Extra
scene del 1
scene move 0 1
scene name 0 Renamed
scene play 0
scene stop
scene set 0 blobs 005aff,ff008c 40 4 ff
scene set 0 solid ff0000,00ff00,0000ff,ffffff,000000 0 0 ff
fseq list
fseq play show.fseq
fseq seek 5000
fseq stop
loglevel warn
rollback ack
factory-reset
reboot""".splitlines()

# fuzz_web_api: byte 0 = route index (kRoutes order), byte 1 = wildcard index.
ROUTES = ["/api/config", "/api/global", "/api/channel", "/api/restore", "/api/scene",
          "DEL /api/scene", "/api/scenes/add", "/api/scenes/move", "/api/scenes/stop",
          "/api/rollback/ack", "/api/autopatch", "/api/fseq/play", "/api/fseq/stop",
          "/api/loglevel", "/api/ota"]
R = {r: i for i, r in enumerate(ROUTES)}
WEB = [
    (R["/api/global"], 0, '{"refresh_hz":45,"short_name":"rig-a"}'),
    (R["/api/global"], 0, '{"web_enabled":true,"sacn_enabled":true,"web_password":"pa55"}'),
    (R["/api/global"], 0, '{"failsafe_mode":2,"failsafe_color":[1,2,3],"scene_mask":255}'),
    (R["/api/channel"], 2, '{"pixel_count":321,"protocol":"WS2812B"}'),
    (R["/api/channel"], 3, '{"protocol":"APA102","pixel_count":77,"gaps":[[5,1],[40,3]]}'),
    (R["/api/channel"], 0x80, '{"pixel_count":999,"color_order":"GRB","brightness":200}'),
    (R["/api/scene"], 0, '{"name":"Blobs","effect":3,"speed":40,"param":4,'
                         '"colors":[[0,90,255],[255,0,140]]}'),
    (R["DEL /api/scene"], 1, ""),
    (R["/api/scenes/add"], 0, '{"name":"Web","effect":3}'),
    (R["/api/scenes/move"], 0, '{"from":3,"to":0}'),
    (R["/api/scenes/stop"], 0, "{}"),
    (R["/api/rollback/ack"], 0, "{}"),
    (R["/api/autopatch"], 0, '{"base":0}'),
    (R["/api/fseq/play"], 0, '{"filename":"show.fseq"}'),
    (R["/api/loglevel"], 0, '{"level":"warn"}'),
    (R["/api/ota"], 0xC0, "\xe9" + "\0" * 300),
    # First fuzzer find: out-of-range numbers were cast before the range check.
    (R["/api/channel"], 1, '{"brightness":-5,"pixel_count":1e40,"grouping":-1}'),
    (R["/api/global"], 0, '{"refresh_hz":-60,"home_timeout_s":1e300}'),
]


def fetch_docs(host_bin):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    proc = subprocess.Popen([host_bin, "--port", str(port)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                get = lambda p: urllib.request.urlopen(f"http://127.0.0.1:{port}{p}",
                                                       timeout=1).read().decode()
                return get("/api/backup"), get("/api/config")
            except OSError:
                time.sleep(0.05)
    finally:
        proc.terminate()
        proc.wait()
    return None


def write(target, blobs):
    d = os.path.join(HERE, "corpus", target)
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        os.remove(os.path.join(d, f))
    for b in blobs:
        b = b.encode("latin-1") if isinstance(b, str) else b
        with open(os.path.join(d, hashlib.sha1(b).hexdigest()[:16]), "wb") as f:
            f.write(b)
    print(f"{target}: {len(blobs)} seeds")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        REPO, "build", "tests", "harness", "pixfrog_api_host")
    web = [bytes([r, i]) + body.encode("latin-1") for r, i, body in WEB]
    docs = fetch_docs(host) if os.path.exists(host) else None
    if docs:
        backup, config = docs
        web += [bytes([R["/api/restore"], 0]) + backup.encode(),
                bytes([R["/api/config"], 0]) + config.encode()]
        json.loads(backup)
    else:
        print(f"(no {host}: backup/config seeds skipped)")
    write("fuzz_parsers", PARSERS)
    write("fuzz_receivers", RECEIVERS)
    write("fuzz_console", [line.encode() for line in CONSOLE])
    write("fuzz_web_api", web)


if __name__ == "__main__":
    main()
