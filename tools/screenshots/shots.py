#!/usr/bin/env python3
"""Documentation screenshots, reproducible: one command, every UI image.

    tools/screenshots/shots.py                    # the images the site uses → docs/img
    tools/screenshots/shots.py --all              # every screen, web (EN+FR) and device
    tools/screenshots/shots.py --only web --out /tmp/shots

Web UI: the real API (`pixfrog_api_host --demo`, a show-sized setup kept
alive) in headless Chromium (Playwright), one PNG per screen and language.
Device: the SDL emulator (NV3007 and ST7789 builds) walked to each screen.

Needs: build/tests/harness/pixfrog_api_host, tools/emulator/build*/pixfrog_emu
(cmake builds of tests/ and tools/emulator), `pip install playwright pillow`
and `python3 -m playwright install chromium`.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
API_HOST = os.path.join(REPO, "build", "tests", "harness", "pixfrog_api_host")
EMULATORS = {
    "nv3007": os.path.join(REPO, "tools", "emulator", "build", "pixfrog_emu"),
    "st7789": os.path.join(REPO, "tools", "emulator", "build.st7789", "pixfrog_emu"),
}

# What the site and the docs show, per UI language (index-{en,fr}.html,
# docs/SHOW_CONTROL.md); --all takes every screen below.
SITE_WEB    = {"en": {"dashboard", "scenes", "effects", "profiles", "channel-control", "control"},
               "fr": {"dashboard", "scenes"}}
SITE_DEVICE = {("nv3007", "home")}

# Web screens: (file stem, nav target, optional element to click before the shot).
WEB = [
    ("dashboard", "dashboard", None),
    ("scenes", "scenes", '[data-scene-row="0"]'),
    ("effects", "effects", '[data-fx-row="8"]'),
    ("fseq", "fseq", None),
    ("channels", "channels", None),
    ("channel-control", "channels", '[data-chan="6"]'),  # an output in DMX control mode
    ("profiles", "profiles", '[data-pf-row="2"]'),
    ("control", "control", None),
    ("network", "network", None),
    ("artnet", "artnet", None),
    ("system", "system", None),
    ("diag", "diag", None),
]

# Device screens: (file stem, menu path from HOME — one cursor per level).
DEVICE = [
    ("home", []),
    ("menu", [0]),
    ("channel", [0, 0]),
    ("scenes", [0, 11, 0]),
    ("control", [0, 8, 6]),
]
DEVICE_SEED = [
    "set chan 0 1 1 300", "set chan 1 1 3 300", "set chan 2 2 5 144", "set chan 3 4 7 120",
    "set chan 4 6 9 240", "set chan 5 3 11 50", "set chan 6 1 13 512", "set chan 7 0 15 10",
    "set net connected", "set ip 192.168.2.50", "set fps 60", "set pkts 1843200",
    "set global web 1", "set global sacn 1",
] + [f"set active {c}" for c in (0, 1, 2, 3, 6)]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def shoot_web(out, langs, every):
    from playwright.sync_api import sync_playwright

    if not os.path.exists(API_HOST):
        sys.exit(f"missing {API_HOST} — cmake --build build/tests --target pixfrog_api_host")
    port = free_port()
    host = subprocess.Popen([API_HOST, "--port", str(port), "--demo"],
                            stdout=subprocess.DEVNULL)
    url = f"http://127.0.0.1:{port}"
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(url + "/api/status", timeout=0.2)
                break
            except OSError:
                time.sleep(0.1)
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            for lang in langs:
                body = json.dumps({"lang": 1 if lang == "fr" else 0}).encode()
                urllib.request.urlopen(urllib.request.Request(
                    url + "/api/global", data=body, method="POST",
                    headers={"Content-Type": "application/json"}))
                page = browser.new_page(viewport={"width": 1440, "height": 900})
                page.goto(url + "/")
                page.wait_for_selector("[data-screen-title]")
                time.sleep(8)  # status polls: live numbers and the sparklines filled
                for stem, nav, action in WEB:
                    if not every and stem not in SITE_WEB[lang]:
                        continue
                    page.locator(f'div[data-nav="{nav}"]').click()
                    if action:
                        page.locator(action).first.click()
                    time.sleep(0.6)  # transitions settle
                    suffix = "" if lang == "en" else "-fr"
                    path = os.path.join(out, f"web-{stem}{suffix}.png")
                    page.screenshot(path=path)
                    print("wrote", os.path.relpath(path, REPO))
                page.close()
            browser.close()
    finally:
        host.terminate()
        host.wait()


def shoot_device(out, every):
    from PIL import Image

    sys.path.insert(0, os.path.join(REPO, "tools", "emulator"))
    from crawl import Emu, goto  # noqa: E402

    for panel, binary in EMULATORS.items():
        if not every and all(p != panel for p, _ in SITE_DEVICE):
            continue
        if not os.path.exists(binary):
            print(f"skip {panel}: no emulator at {os.path.relpath(binary, REPO)}")
            continue
        emu = Emu(binary)
        try:
            for c in DEVICE_SEED:
                emu.cmd(c)
            for stem, path in DEVICE:
                if not every and (panel, stem) not in SITE_DEVICE:
                    continue
                goto(emu, path)
                bmp = os.path.join(out, f".{panel}-{stem}.bmp")
                emu.shot(bmp)
                png = os.path.join(out, f"ui-{panel}-{stem}.png")
                Image.open(bmp).convert("RGB").save(png, optimize=True)
                os.remove(bmp)
                print("wrote", os.path.relpath(png, REPO))
        finally:
            emu.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(REPO, "docs", "img"))
    ap.add_argument("--only", choices=["web", "device"])
    ap.add_argument("--lang", choices=["en", "fr"], help="web UI language (default both)")
    ap.add_argument("--all", action="store_true", help="every screen, not just the site's")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.only != "device":
        shoot_web(args.out, [args.lang] if args.lang else ["en", "fr"], args.all)
    if args.only != "web":
        shoot_device(args.out, args.all)


if __name__ == "__main__":
    main()
