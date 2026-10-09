#!/usr/bin/env python3
"""Two boxes on one LAN: the hub (tabs, overview, a sibling's UI) and the
pixfrog.local hand-over — the preferred box takes the alias, and when the
holder vanishes the other one takes it back (~45 s).

The serial board (PORT, BOARD_IP) and its sibling (OTHER_IP), both with the
web UI on and no password. Both boxes' hub_preferred are restored. Not in
run_all's default list."""
import json, os, sys, time, urllib.request
from pixfrog_uart import Board, Checks, BOARD_IP, main_guard

OTHER = os.environ.get("OTHER_IP", "")


def get(ip, path):
    try:
        with urllib.request.urlopen(f"http://{ip}{path}", timeout=2) as r:
            return json.loads(r.read())
    except Exception:
        return None


def post_global(ip, body):
    req = urllib.request.Request(f"http://{ip}/api/global", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return r.status


def alias(ip):
    st = get(ip, "/api/status")
    return st.get("alias") if st else None


def wait(cond, limit):
    t0 = time.time()
    while time.time() - t0 < limit:
        if cond():
            return time.time() - t0
        time.sleep(1)
    return None


def run(board: Board):
    c = Checks("multiboard")
    if not OTHER:
        c.check("OTHER_IP set (the sibling's address)", False)
        return c.finish()
    me, other = BOARD_IP, OTHER
    c.check("link up", board.wait_link())
    c.check("both see each other", wait(lambda: all(
        isinstance(get(ip, "/api/peers"), list) and len(get(ip, "/api/peers")) == 2 for ip in (me, other)), 60) is not None)
    with urllib.request.urlopen(f"http://{me}/api/status", timeout=2) as r:
        cors = r.headers.get("Access-Control-Allow-Origin")
    c.check(f"status readable cross-origin (Access-Control-Allow-Origin: {cors})", cors == "*")
    c.check("exactly one box holds pixfrog.local",
            wait(lambda: [alias(me), alias(other)].count(True) == 1, 60) is not None)

    other_pref = (get(other, "/api/config") or {}).get("global", {}).get("hub_preferred")
    me_pref = board.get("global", "hub_preferred")
    try:
        post_global(other, {"hub_preferred": False})
        board.cmd("global hub_preferred 1")
        t = wait(lambda: alias(me) is True and alias(other) is False, 60)
        c.check(f"preferred → this box holds the alias ({t and round(t)} s)", t is not None)

        board.cmd("global web_enabled 0")  # the holder vanishes from mDNS
        t = wait(lambda: alias(other) is True, 120)
        c.check(f"holder gone → the sibling takes it ({t and round(t)} s, ≤ ~60)", t is not None and t <= 62)
    finally:
        board.cmd("global web_enabled 1")
        board.cmd(f"global hub_preferred {me_pref}")
        post_global(other, {"hub_preferred": bool(other_pref)})
    t = wait(lambda: [alias(me), alias(other)].count(True) == 1, 60)
    c.check(f"back: one holder again ({t and round(t)} s)", t is not None)
    return c.finish()


if __name__ == "__main__":
    sys.exit(main_guard(run))
