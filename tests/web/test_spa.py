"""Browser tests of the embedded SPA against the real API.

Each test launches tests/web/api_host (the firmware's web_config handlers
compiled for the host, fresh factory state) and drives the page with
Playwright/Chromium, then checks the device state through the same API.

    python3 -m pytest tests/web -q
    PIXFROG_API_HOST=path/to/pixfrog_api_host python3 -m pytest tests/web

Needs `pip install pytest playwright && python3 -m playwright install chromium`.
"""
import json
import os
import socket
import subprocess
import time
import urllib.request

import pytest
from playwright.sync_api import expect, sync_playwright

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOST_BIN = os.environ.get(
    "PIXFROG_API_HOST", os.path.join(REPO, "build", "tests", "harness", "pixfrog_api_host"))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Device:
    def __init__(self, *args):
        self.port = free_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.proc = subprocess.Popen([HOST_BIN, "--port", str(self.port), *args],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                urllib.request.urlopen(self.url + "/api/status", timeout=0.5)
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("api host did not start")

    def get(self, path):
        with urllib.request.urlopen(self.url + path, timeout=5) as r:
            return json.loads(r.read())

    def post(self, path, body):
        req = urllib.request.Request(self.url + path, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())

    def close(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)


@pytest.fixture(scope="session")
def browser():
    if not os.path.exists(HOST_BIN):
        pytest.skip(f"api host not built: {HOST_BIN}")
    with sync_playwright() as p:
        b = p.chromium.launch()
        yield b
        b.close()


@pytest.fixture
def device(request):
    marker = request.node.get_closest_marker("device_args")
    d = Device(*(marker.args if marker else ()))
    yield d
    d.close()


@pytest.fixture
def page(browser, device):
    ctx = browser.new_context(viewport={"width": 1440, "height": 900})
    pg = ctx.new_page()
    errors = []
    pg.on("pageerror", lambda e: errors.append(str(e)))
    pg.goto(device.url + "/")
    expect(pg.locator("[data-screen-title]")).to_have_text("Dashboard")
    yield pg
    ctx.close()
    assert not errors, f"JavaScript errors: {errors}"


def nav(page, screen):
    page.locator(f'div[data-nav="{screen}"]').click()


def save(page):
    page.locator('[data-action="save"]').click()
    expect(page.locator('[data-live="save-state"]')).to_contain_text("saved")


def drag(page, handle, target):
    """Pointer drag from a grip to the centre of another element."""
    hb, tb = handle.bounding_box(), target.bounding_box()
    page.mouse.move(hb["x"] + hb["width"] / 2, hb["y"] + hb["height"] / 2)
    page.mouse.down()
    page.mouse.move(tb["x"] + tb["width"] / 2, tb["y"] + tb["height"] / 2, steps=8)
    page.mouse.up()


# ── dashboard ────────────────────────────────────────────────────────────────

def test_dashboard_shows_the_channels(page):
    expect(page.locator("#dash-chan-mount > div")).to_have_count(8)
    expect(page.locator('[data-live="chan-count"]')).to_contain_text("/ 1")


# ── scenes ───────────────────────────────────────────────────────────────────

def test_add_rename_and_delete_a_scene(page, device):
    nav(page, "scenes")
    rows = page.locator("[data-scene-row]")
    expect(rows).to_have_count(8)
    page.locator('[data-action="scene-add"]').click()
    expect(rows).to_have_count(9)
    name = page.locator("[data-sc-name]")
    name.fill("From test")
    save(page)
    assert device.get("/api/config")["scenes"][8]["name"] == "From test"

    page.locator('[data-action="scene-del"]').click()
    page.locator("[data-modal-confirm]").click()
    expect(rows).to_have_count(8)
    assert len(device.get("/api/config")["scenes"]) == 8


def test_drag_a_scene_to_reorder_the_list(page, device):
    nav(page, "scenes")
    first = device.get("/api/config")["scenes"][0]["name"]
    drag(page, page.locator('[data-scene-row="0"] [data-srow-grip]'),
         page.locator('[data-scene-row="3"]'))
    expect(page.locator('[data-scene-row="3"]')).to_contain_text(first)
    time.sleep(0.3)
    assert device.get("/api/config")["scenes"][3]["name"] == first


def test_scene_colours_add_reorder_remove_middle(page, device):
    nav(page, "scenes")
    page.locator('[data-scene-row="3"]').click()  # Blobs: 3 colours
    chip = lambda k: page.locator(f'[data-scene-color="4"][data-k="{k}"]')
    before = device.get("/api/config")["scenes"][3]["colors"]
    assert len(before) == 3
    drag(page, chip(0).locator("[data-sc-grip]"), chip(2))
    page.locator('[data-sc-delcol="4"][data-k="1"]').click()  # the middle one
    page.locator('[data-sc-addcol="4"]').click()
    save(page)
    after = device.get("/api/config")["scenes"][3]["colors"]
    assert after[0] == before[1] and after[1] == before[0] and len(after) == 3


def test_effect_dropdown_and_param_slider(page, device):
    nav(page, "scenes")
    page.locator("[data-sc-fxsel]").select_option(label="Fire")
    expect(page.locator("[data-sc-param]")).to_have_count(0)  # fire has no param
    page.locator("[data-sc-fxsel]").select_option(label="Twinkle")
    page.locator("[data-sc-param]").fill("120")
    save(page)
    s = device.get("/api/config")["scenes"][0]
    assert s["effect"] == 6 and s["param"] == 120


# ── channels ─────────────────────────────────────────────────────────────────

def test_pixel_count_above_budget_warns_and_is_kept(page, device):
    nav(page, "channels")
    page.locator("#cd-pix").fill("1024")
    expect(page.locator("#cd-pixwarn")).to_be_visible()
    save(page)
    ch = device.get("/api/config")["channels"][0]
    assert ch["pixel_count"] == 1024 and ch["max_pixels"] == 512


def test_dead_pixel_gaps_editor(page, device):
    nav(page, "channels")
    page.locator("[data-gap-add]").click()
    page.locator("#cd-gap-p-0").fill("12")
    page.locator("#cd-gap-l-0").fill("2")
    save(page)
    assert device.get("/api/config")["channels"][0]["gaps"] == [[12, 2]]


# ── system / global ──────────────────────────────────────────────────────────

def test_refresh_rate_input(page, device):
    nav(page, "system")
    page.locator("#s-refresh").fill("45")
    save(page)
    assert device.get("/api/config")["global"]["refresh_hz"] == 45


def test_french_translation(page):
    nav(page, "system")
    page.locator("#s-lang").select_option("fr")
    nav(page, "dashboard")
    expect(page.locator("[data-screen-title]")).to_have_text("Tableau de bord")


@pytest.mark.device_args("--password", "s3cret")
def test_password_prompt_on_write(page, device):
    page.on("dialog", lambda d: d.accept("s3cret"))
    nav(page, "system")
    page.locator("#s-refresh").fill("50")
    save(page)
    assert device.get("/api/config")["global"]["refresh_hz"] == 50


# ── rollback banner ──────────────────────────────────────────────────────────

@pytest.mark.device_args("--rollback")
def test_rollback_banner_until_acknowledged(page, device):
    banner = page.locator("#rb-banner")
    expect(banner).to_be_visible()
    expect(banner).to_contain_text("v9.9.9-bad")
    page.locator('[data-action="rollback-ack"]').click()
    expect(banner).to_be_hidden()
    assert "rollback" not in device.get("/api/status")
