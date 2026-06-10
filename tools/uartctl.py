#!/usr/bin/env python3
"""Send control-console commands to the board and print the responses.

Usage: uartctl.py [-p PORT] [-q] "cmd with args" [...]
Exit code 1 if any command answered ERR or timed out.

Opening the port may auto-reset the board (bridge DTR/RTS circuit), so we
poke newlines until the pixfrog> prompt answers before sending anything.
"""

import argparse
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*m")
TERM = re.compile(r"^(OK|ERR .*?)\r?$", re.M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("-q", "--quiet-logs", action="store_true",
                    help="send 'loglevel none' first")
    ap.add_argument("cmds", nargs="+")
    args = ap.parse_args()

    s = serial.Serial()
    s.port = args.port
    s.baudrate = 115200
    s.timeout = 1
    s.dtr = False
    s.rts = False
    s.open()

    buf = ""
    deadline = time.time() + 15
    while "pixfrog>" not in buf:
        if time.time() > deadline:
            sys.exit("ERR no pixfrog> prompt (is the firmware running?)")
        s.write(b"\r\n")
        time.sleep(0.5)
        buf += s.read(s.in_waiting or 1).decode(errors="replace")
    s.reset_input_buffer()

    cmds = (["loglevel none"] if args.quiet_logs else []) + args.cmds
    rc = 0
    for c in cmds:
        s.write((c + "\r\n").encode())
        out = ""
        m = None
        deadline = time.time() + 3
        while time.time() < deadline:
            out += s.read(s.in_waiting or 1).decode(errors="replace")
            m = TERM.search(ANSI.sub("", out))
            if m:
                break
        print(f"### {c}")
        for line in ANSI.sub("", out).splitlines():
            line = line.strip()
            if line and not line.startswith("pixfrog>") and line != c:
                print(line)
        if not m:
            print("ERR timeout")
        if not m or m.group(1).startswith("ERR"):
            rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
