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


# ── show control / DMX control / zones ───────────────────────────────────────

def test_show_card_master_and_blackout(page, device):
    page.locator("#sh-master").fill("40")
    page.wait_for_function("() => true")
    for _ in range(40):
        if device.get("/api/status")["show"]["master_local"][0] == 40:
            break
        time.sleep(0.05)
    assert device.get("/api/status")["show"]["master_local"][0] == 40
    page.locator('[data-action="blackout"]').click()
    expect(page.locator("#sh-bo")).to_have_css("background-color", "rgb(217, 83, 79)")
    assert device.get("/api/status")["show"]["blackout"] == 255


def test_control_editor_preset_zone_and_save(page, device):
    nav(page, "control")
    page.locator('[data-ct-preset="full"]').click()
    rows = page.locator("[data-ct-row]")
    expect(rows).to_have_count(15)
    expect(page.locator("#ct-foot")).to_contain_text("16 ch")
    for o in range(4, 8):  # the scene channel (row 3) plays on outputs 1-4 only
        page.locator(f'[data-ct-out="3"][data-o="{o}"]').click()
    page.locator('[data-ct-fn="4"]').select_option("fseq")
    page.locator("#ct-en").check(force=True)
    page.locator("#ct-uni").fill("77")
    save(page)
    c = device.get("/api/config")["control"]
    assert c["enabled"] and c["universe"] == 77 and len(c["slots"]) == 15
    assert c["slots"][3] == {"fn": "scene", "mask": 15, "index": 0, "fine": False}
    assert c["slots"][4]["fn"] == "fseq"
    expect(page.locator("#ct-state")).to_contain_text("waiting")


def test_control_overlap_and_overflow_warnings(page, device):
    nav(page, "control")
    page.locator("#ct-en").check(force=True)
    page.locator("#ct-uni").fill("1")  # channel 1's universe
    expect(page.locator("#ct-warn")).to_contain_text("also feeds channel 1")
    page.locator("#ct-addr").fill("510")  # 6 channels from 510 pass 512
    expect(page.locator("#ct-warn")).to_contain_text("past DMX channel 512")


def test_fixture_profile_download(page, device):
    nav(page, "control")
    with page.expect_download() as dl:
        page.locator("#ct-fixture").click()
    assert dl.value.suggested_filename == "pixfrog-control.json"
    profile = json.loads(open(dl.value.path()).read())
    assert profile["modes"][0]["channels"][:2] == ["Master", "Master fine"]


def test_zones_show_on_the_scene_list(page, device):
    device.post("/api/scene/0/play", {"outputs": 15})
    device.post("/api/scene/1/play", {"outputs": 240})
    nav(page, "scenes")
    expect(page.locator('[data-sc-outs="0"]')).to_contain_text("out 1,2,3,4")
    expect(page.locator('[data-sc-outs="1"]')).to_contain_text("out 5,6,7,8")
    page.locator('[data-scene-row="0"] [data-sc-play]').click()  # playing: stops it
    expect(page.locator('[data-sc-outs="0"]')).to_have_count(0)
    assert device.get("/api/status")["show"]["scenes"][:4] == [-1, -1, -1, -1]


def test_scene_fade_setting(page, device):
    nav(page, "system")
    page.locator("#s-fade").fill("1.5")
    save(page)
    assert device.get("/api/config")["global"]["scene_fade_ms"] == 1500


def _play_on(page, row, outputs):
    """Select scene `row`, keep only `outputs` (0-based) in PLAY ON, press Play."""
    page.locator(f'[data-scene-row="{row}"]').click()
    n = row + 1
    for o in range(8):
        chip = page.locator(f'[data-sc-pon="{o}"][data-scn="{n}"]')
        on = "rgba(63,212,99" in (chip.get_attribute("style") or "")  # the lit chip style
        if (o in outputs) != on:
            chip.click()
    page.locator(f'[data-sc-playon="{n}"]').click()


def test_play_two_scenes_on_two_outputs_from_the_editor(page, device):
    nav(page, "scenes")
    _play_on(page, 0, {0})
    expect(page.locator('[data-sc-playing="1"]')).to_contain_text("out 1")
    _play_on(page, 1, {1})
    expect(page.locator('[data-sc-outs="1"]')).to_contain_text("out 2")
    scenes = device.get("/api/status")["show"]["scenes"]
    assert scenes[:3] == [0, 1, -1]
    page.locator('[data-sc-stop="2"]').click()  # stops scene 2 only
    for _ in range(40):
        if device.get("/api/status")["show"]["scenes"][:2] == [0, -1]:
            break
        time.sleep(0.05)
    assert device.get("/api/status")["show"]["scenes"][:2] == [0, -1]


def test_play_on_offers_only_the_scene_target_channels(page, device):
    device.post("/api/scene/0", {"mask": 3})
    page.reload()
    nav(page, "scenes")
    page.locator('[data-scene-row="0"]').click()
    off = page.locator('[data-sc-pon="5"][data-scn="1"]')
    assert "not-allowed" in off.get_attribute("style")
    off.click()  # not a target channel: nothing to toggle
    page.locator('[data-sc-playon="1"]').click()
    for _ in range(40):
        if device.get("/api/status")["show"]["scenes"][:3] == [0, 0, -1]:
            break
        time.sleep(0.05)
    assert device.get("/api/status")["show"]["scenes"][:3] == [0, 0, -1]
