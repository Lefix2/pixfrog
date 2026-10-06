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
import re
import socket
import subprocess
import sys
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
        # pixfrog.local → this machine, so the hub tests can open the alias.
        b = p.chromium.launch(args=["--host-resolver-rules=MAP pixfrog.local 127.0.0.1"])
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
    expect(handle).to_be_visible()  # a list re-rendered after a save has no box yet
    expect(target).to_be_visible()
    hb, tb = handle.bounding_box(), target.bounding_box()
    page.mouse.move(hb["x"] + hb["width"] / 2, hb["y"] + hb["height"] / 2)
    page.mouse.down()
    page.mouse.move(tb["x"] + tb["width"] / 2, tb["y"] + tb["height"] / 2, steps=8)
    page.mouse.up()


# ── dashboard ────────────────────────────────────────────────────────────────

# The sidebar: five groups, every screen in its group, each one opening.
def test_the_menu_is_grouped_by_job(page):
    groups = page.evaluate("""() => { const out = []; let cur = [''];
        for (const el of document.querySelector('aside').children) {
          if (el.hasAttribute('data-nav')) { if (cur.length === 1 && !out.length) out.push(cur); cur.push(el.getAttribute('data-nav')); }
          else if (el.hasAttribute('data-i18n')) { cur = [el.getAttribute('data-i18n')]; out.push(cur); }
        }
        return out; }""")
    # The dashboard on its own, then the groups in the order a rig is built:
    # each screen only refers to what sits above it.
    assert groups == [
        ["", "dashboard"],
        ["RIG", "channels", "groups"],
        ["LOOKS", "effects", "scenes"],
        ["DMX", "artnet", "profiles", "dmxpatch", "control", "patch"],  # auto-patch last
        ["PLAYBACK", "fseq", "auto"],
        ["SETTINGS", "network", "system", "maint"]]
    titles = {"auto": "Automations", "dmxpatch": "Output patch", "maint": "Maintenance",
              "channels": "Outputs", "artnet": "Protocols", "control": "Control universe"}
    for g in groups:
        for screen in g[1:]:
            nav(page, screen)
            expect(page.locator(f'[data-screen="{screen}"]')).to_be_visible()
            if screen in titles:
                expect(page.locator("[data-screen-title]")).to_have_text(titles[screen])
    # What moved is where the tree says: startup, fades and signal loss
    # together; firmware and backup under maintenance; the frame rate with the
    # outputs; the DMX address with the patch.
    for screen, ids in (("auto", ["s-boot", "pl-auto", "s-fade", "a-fsmode"]),
                        ("maint", ["ota-file", "restore-file", "d-logs", "d-chans"]),  # telemetry and logs too
                        ("channels", ["s-refresh", "cd-on", "cd-proto", "cd-pix", "cd-gaps"]),
                        ("dmxpatch", ["cd-pixmap", "cd-pack", "cd-uni", "cd-dmx", "cd-fixctl", "cd-funi", "cd-fdmx"]),
                        ("system", ["s-lang", "s-bright", "s-home"])):
        for i in ids:
            assert page.evaluate(
                "(a) => document.getElementById(a[1]).closest('[data-screen]').getAttribute('data-screen') === a[0]",
                [screen, i]), f"#{i} is not on the {screen} screen"


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


def test_scene_parts_send_an_effect_to_each_output_group(page, device):
    nav(page, "scenes")
    expect(page.locator("[data-pt-row]")).to_have_count(1)  # the default: one effect everywhere
    expect(page.locator("[data-pt-add]")).to_have_count(0)  # no output left for another part
    for o in (4, 5, 6, 7):
        page.locator(f'[data-pt-out="0"][data-o="{o}"]').click()
    page.locator("[data-pt-add]").click()  # takes the outputs left free
    expect(page.locator("[data-pt-row]")).to_have_count(2)
    page.locator('[data-pt-fx="1"]').select_option("2")
    page.locator('[data-pt-mode="1"]').select_option("chain")
    page.locator('[data-pt-out="1"][data-o="0"]').click()  # output 1 moves to the second part
    save(page)
    parts = device.get("/api/config")["scenes"][0]["parts"]
    assert parts == [{"mask": 0x0E, "effect": 0, "fixture_mode": "each", "reverse": False},
                     {"mask": 0xF1, "effect": 2, "fixture_mode": "chain", "reverse": False}]
    expect(page.locator('[data-scene-row="0"]')).to_contain_text("Warm white + 1")

    page.locator('[data-pt-del="0"]').click()
    save(page)
    assert device.get("/api/config")["scenes"][0]["parts"] == [
        {"mask": 0xF1, "effect": 2, "fixture_mode": "chain", "reverse": False}]


def test_scene_fixture_mode(page, device):
    nav(page, "scenes")
    page.locator("[data-pt-mode]").first.select_option("mirror")
    save(page)
    assert device.get("/api/config")["scenes"][0]["parts"][0]["fixture_mode"] == "mirror"


# ── effects ──────────────────────────────────────────────────────────────────

def test_add_rename_duplicate_and_delete_an_effect(page, device):
    nav(page, "effects")
    rows = page.locator("[data-fx-row]")
    expect(rows).to_have_count(8)
    page.locator('[data-action="fx-add"]').click()
    expect(rows).to_have_count(9)
    page.locator("[data-fx-name]").fill("From test")
    save(page)
    assert device.get("/api/config")["effects"][8]["name"] == "From test"
    expect(page.locator("[data-fx-users]")).to_contain_text("No scene plays this effect")

    page.locator('[data-action="fx-dup"]').click()
    expect(rows).to_have_count(10)
    fx = device.get("/api/config")["effects"]
    assert fx[9]["name"] == "From test copy" and fx[9]["colors"] == fx[8]["colors"]

    page.locator('[data-action="fx-del"]').click()
    page.locator("[data-modal-confirm]").click()
    expect(rows).to_have_count(9)

    # An effect a scene plays stays: the scenes would lose their look.
    page.locator('[data-fx-row="1"]').click()
    expect(page.locator("[data-fx-users]")).to_contain_text("Played by scene 2")
    page.locator('[data-action="fx-del"]').click()
    expect(page.locator("[data-modal-confirm]")).not_to_be_visible()
    assert len(device.get("/api/config")["effects"]) == 9


def test_effect_preview_plays_what_the_box_draws(page, device):
    nav(page, "effects")
    px = "(function(){var c=document.querySelector('canvas[data-fx-preview]');" \
         "var d=c.getContext('2d').getImageData(0,0,c.width,1).data;return [c.width].concat(Array.from(d));})()"

    def strip():
        d = page.evaluate(px)
        return d[0], [tuple(d[1 + i * 4:4 + i * 4]) for i in range(d[0])]

    def wait_for(pred):
        for _ in range(60):
            n, leds = strip()
            if pred(n, leds):
                return n, leds
            time.sleep(0.05)
        raise AssertionError(f"preview never matched: {strip()}")

    # Effect 1 is the factory warm white: every LED of the strip shows it.
    n, leds = wait_for(lambda n, leds: leds[0] == (255, 180, 110))
    assert n == 60 and set(leds) == {(255, 180, 110)}
    # Edited, not saved: the preview follows, the box keeps its effect.
    page.locator("[data-fx-gen]").select_option("2")  # rainbow
    wait_for(lambda n, leds: len(set(leds)) > 10)
    assert device.get("/api/config")["effects"][0]["generator"] == 0
    page.locator('[data-fx-row="1"]').click()  # the factory chase: it moves
    _, first = wait_for(lambda n, leds: (255, 255, 255) in leds and (0, 0, 0) in leds)
    wait_for(lambda n, leds: leds != first and (255, 255, 255) in leds)
    page.locator("[data-fx-pvsize]").select_option("144")
    wait_for(lambda n, leds: n == 144)
    nav(page, "scenes")  # leaving the screen stops the requests
    time.sleep(0.3)
    seen = []
    page.on("request", lambda r: seen.append(r.url) if "/api/effect/preview" in r.url else None)
    time.sleep(1.5)
    assert not seen


def test_drag_an_effect_and_the_scenes_follow(page, device):
    nav(page, "effects")
    first = device.get("/api/config")["effects"][0]["name"]
    drag(page, page.locator('[data-fx-row="0"] [data-frow-grip]'),
         page.locator('[data-fx-row="3"]'))
    expect(page.locator('[data-fx-row="3"]')).to_contain_text(first)
    time.sleep(0.3)
    cfg = device.get("/api/config")
    assert cfg["effects"][3]["name"] == first
    assert cfg["scenes"][0]["parts"][0]["effect"] == 3  # scene 1 kept its look
    assert cfg["scenes"][1]["parts"][0]["effect"] == 0


def test_effect_colours_add_reorder_remove_middle(page, device):
    nav(page, "effects")
    page.locator('[data-fx-row="3"]').click()  # Blobs: 3 colours
    chip = lambda k: page.locator(f'[data-fx-color="4"][data-k="{k}"]')
    before = device.get("/api/config")["effects"][3]["colors"]
    assert len(before) == 3
    drag(page, chip(0).locator("[data-fx-grip]"), chip(2))
    page.locator('[data-fx-delcol="4"][data-k="1"]').click()  # the middle one
    page.locator('[data-fx-addcol="4"]').click()
    save(page)
    after = device.get("/api/config")["effects"][3]["colors"]
    assert after[0] == before[1] and after[1] == before[0] and len(after) == 3


def test_generator_dropdown_and_param_slider(page, device):
    nav(page, "effects")
    page.locator("[data-fx-gen]").select_option(label="Fire")
    expect(page.locator("[data-fx-param]")).to_have_count(0)  # fire has no param
    page.locator("[data-fx-gen]").select_option(label="Twinkle")
    page.locator("[data-fx-param]").fill("120")
    page.locator("[data-fx-speed]").fill("200")
    save(page)
    e = device.get("/api/config")["effects"][0]
    assert e["generator"] == 6 and e["param"] == 120 and e["speed"] == 200


def test_dimmer_phaser_and_invert(page, device):
    nav(page, "effects")
    expect(page.locator("[data-fx-phrate]")).to_have_count(0)  # no phaser: no phaser sliders
    page.locator("[data-fx-wave]").select_option("sin")
    page.locator("[data-fx-phrate]").fill("20")
    expect(page.locator("[data-fx-phrateval]")).to_contain_text("1.00 Hz · 60 BPM")
    page.locator("[data-fx-phspread]").fill("16")
    expect(page.locator("[data-fx-phspreadval]")).to_contain_text("1 cycle")
    page.locator("[data-fx-phwidth]").fill("128")
    page.locator("[data-fx-phlow]").fill("51")
    expect(page.locator("[data-fx-phlowval]")).to_contain_text("20 %")
    page.locator("[data-fx-phrev]").check(force=True)
    page.locator("[data-fx-invert]").check(force=True)
    save(page)
    e = device.get("/api/config")["effects"][0]
    assert e["phaser"] == {"wave": "sin", "rate": 20, "spread": 16, "width": 128, "low": 51,
                           "attack": 0, "decay": 0, "reverse": True}
    assert e["invert"] is True
    # Reloaded from the box, the card shows what was saved.
    expect(page.locator("[data-fx-wave]")).to_have_value("sin")
    expect(page.locator("[data-fx-phrate]")).to_have_value("20")
    expect(page.locator("[data-fx-invert]")).to_be_checked()

    # A PWM gets its attack and decay: the edges of the lit part.
    expect(page.locator("[data-fx-phattack]")).to_have_count(0)
    page.locator("[data-fx-wave]").select_option("pwm")
    expect(page.locator("[data-fx-phattackval]")).to_contain_text("hard edge")
    page.locator("[data-fx-phattack]").fill("64")
    expect(page.locator("[data-fx-phattackval]")).to_contain_text("25 %")
    page.locator("[data-fx-phdecay]").fill("128")
    save(page)
    ph = device.get("/api/config")["effects"][0]["phaser"]
    assert ph["wave"] == "pwm" and ph["attack"] == 64 and ph["decay"] == 128

    page.locator("[data-fx-wave]").select_option("none")
    page.locator("[data-fx-invert]").uncheck(force=True)
    save(page)
    e = device.get("/api/config")["effects"][0]
    assert e["phaser"]["wave"] == "none" and e["invert"] is False


def test_block_groups_wings(page, device):
    nav(page, "effects")
    expect(page.locator("[data-fx-mxblockval]")).to_have_text("off")
    page.locator("[data-fx-mxblock]").fill("3")
    expect(page.locator("[data-fx-mxblockval]")).to_have_text("3 px")
    page.locator("[data-fx-mxgroups]").fill("4")
    page.locator("[data-fx-mxwings]").fill("2")
    expect(page.locator("[data-fx-mxwingsval]")).to_have_text("2 parts")
    save(page)
    assert device.get("/api/config")["effects"][0]["matricks"] == {"block": 3, "groups": 4, "wings": 2}
    expect(page.locator("[data-fx-mxgroups]")).to_have_value("4")  # reloaded from the box
    page.locator("[data-fx-mxwings]").fill("1")  # one part is no wings at all
    expect(page.locator("[data-fx-mxwingsval]")).to_have_text("off")
    save(page)
    assert device.get("/api/config")["effects"][0]["matricks"]["wings"] == 0


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
    page.locator('[data-lay-add="d"]').click()  # after the strip as it was
    page.locator("#cd-lay-n-1").fill("2")
    save(page)
    ch = device.get("/api/config")["channels"][0]
    assert ch["gaps"] == [[ch["pixel_count"] + 1, 2]]


def test_the_first_fixture_takes_the_strip_as_it_is(page, device):
    device.post("/api/channel/0", {"protocol": "WS2815", "pixel_count": 60, "fixtures": [], "gaps": []})
    page.reload()
    nav(page, "channels")
    page.locator('[data-lay-add="f"]').click()
    expect(page.locator("[data-lay-row]")).to_have_count(1)  # the fixture, and nothing with it
    expect(page.locator("#cd-lay-n-0")).to_have_value("60")
    expect(page.locator("#cd-pix")).to_have_value("60")  # the strip is no longer
    page.locator('[data-lay-add="f"]').click()  # the next one: as long as the last
    expect(page.locator("#cd-lay-n-1")).to_have_value("60")
    save(page)
    ch = device.get("/api/config")["channels"][0]
    assert ch["fixtures"] == [[1, 60], [61, 60]] and ch["gaps"] == [] and ch["pixel_count"] == 120


def test_fixtures_by_size_in_order_and_reordered_by_drag(page, device):
    nav(page, "channels")
    # 5 bars of 59 LEDs, one dead LED between two bars.
    page.locator("#lay-rep-n").fill("5")
    page.locator("#lay-rep-l").fill("59")
    page.locator("#lay-rep-k").fill("1")
    page.locator("[data-lay-fill]").click()
    expect(page.locator("#cd-pix")).to_have_value("295")
    expect(page.locator("#cd-pix")).to_be_disabled()  # the list sets it
    expect(page.locator("#cd-lay-viz")).to_contain_text("F5")
    save(page)
    ch = device.get("/api/config")["channels"][0]
    assert ch["fixtures"] == [[1, 59], [61, 59], [121, 59], [181, 59], [241, 59]]
    assert ch["gaps"] == [[60, 1], [120, 1], [180, 1], [240, 1]]
    assert ch["pixel_count"] == 295
    # Sizes only: the first bar grows, every later one moves along.
    page.locator("#cd-lay-n-0").fill("70")
    page.locator("#cd-lay-n-0").dispatch_event("change")
    expect(page.locator("#cd-pix")).to_have_value("306")
    # Drag the last bar to the top: positions follow the order.
    page.locator("#cd-lay-n-8").fill("20")
    page.locator("#cd-lay-n-8").dispatch_event("change")
    drag(page, page.locator("[data-lay-row='8'] [data-lay-grip]"), page.locator("[data-lay-row='0']"))
    save(page)
    ch = device.get("/api/config")["channels"][0]
    assert ch["fixtures"][0] == [1, 20] and ch["fixtures"][1] == [21, 70]
    # A fixture mounted the other way round.
    page.locator('[data-lay-rev="0"]').click()
    expect(page.locator("#cd-lay-viz")).to_contain_text("⇄")
    save(page)
    assert device.get("/api/config")["channels"][0]["fixtures"][0] == [1, 20, 1]
    # A row turned into LEDs without fixture leaves the fixtures.
    page.locator("#cd-lay-t-0").select_option("s")
    save(page)
    assert len(device.get("/api/config")["channels"][0]["fixtures"]) == 4


def test_fixture_groups_pick_order_and_save(page, device):
    device.post("/api/channel/0", {"protocol": "WS2815", "pixel_count": 295, "fixtures": [[1, 59], [61, 59], [121, 59], [181, 59], [241, 59]], "gaps": [[60, 1], [120, 1], [180, 1], [240, 1]]})
    page.reload()
    nav(page, "groups")
    page.locator("[data-grp-new]").click()
    page.locator("#grp-name").fill("Top")
    page.locator("#grp-name").dispatch_event("change")
    page.locator('[data-gm-add="0,2"]').click()
    page.locator('[data-gm-all="0"]').click()  # the rest of output 1, after it
    expect(page.locator("[data-gm-row]")).to_have_count(5)
    page.locator('[data-gm-add="0,4"]').click()  # picked again: removed
    page.locator("[data-gm-rev]").click()
    save(page)
    g = device.get("/api/config")["groups"]
    assert g == [{"name": "Top", "members": [[0, 3], [0, 1], [0, 0], [0, 2]]}]
    drag(page, page.locator("[data-gm-row='3'] [data-gm-grip]"), page.locator("[data-gm-row='0']"))
    save(page)
    assert device.get("/api/config")["groups"][0]["members"][0] == [0, 2]


def test_scenes_play_on_groups_and_take_over_only_their_bars(page, device):
    bars = {"protocol": "WS2815", "pixel_count": 20, "fixtures": [[1, 4], [5, 4], [9, 4], [13, 4], [17, 4]], "gaps": []}
    device.post("/api/channel/0", bars)
    device.post("/api/channel/1", bars)
    device.post("/api/groups", {"groups": [
        {"name": "Top", "members": [[0, 4], [0, 3], [0, 2], [0, 1], [0, 0], [1, 0], [1, 1], [1, 2], [1, 3], [1, 4]]},
        {"name": "Centre", "members": [[0, 0], [1, 0]]}]})
    page.reload()
    nav(page, "scenes")
    page.locator('[data-scene-row="0"]').click()
    page.locator('[data-sc-group="1"]').select_option("0")  # scene 1 plays on Top
    page.locator('[data-pt-rev="0"]').check()  # its first part, from the far end
    page.locator('[data-sc-playon="1"]').click()  # saved first, then played
    page.locator('[data-scene-row="1"]').click()
    page.locator('[data-sc-group="2"]').select_option("1")  # scene 2 on Centre
    page.locator('[data-sc-playon="2"]').click()
    for _ in range(40):
        plays = device.get("/api/status")["show"]["plays"]
        if sorted(plays) == [[0, 0], [1, 1]]:
            break
        time.sleep(0.05)
    assert sorted(device.get("/api/status")["show"]["plays"]) == [[0, 0], [1, 1]]
    sc = device.get("/api/config")["scenes"]
    assert sc[0]["group"] == 0 and sc[1]["group"] == 1
    assert sc[0]["parts"][0]["reverse"] is True
    expect(page.locator('[data-sc-playing="2"]')).to_contain_text("Centre")
    page.locator('[data-sc-stop="2"]').click()  # Centre back to Top's scene? no: to its outputs
    for _ in range(40):
        if device.get("/api/status")["show"]["plays"] == [[0, 0]]:
            break
        time.sleep(0.05)
    assert device.get("/api/status")["show"]["plays"] == [[0, 0]]


# ── DMX profiles ─────────────────────────────────────────────────────────────

def test_dmx_profiles_editor(page, device):
    nav(page, "profiles")
    rows = page.locator("[data-pf-row]")
    expect(rows).to_have_count(4)  # the presets
    expect(rows.nth(2)).to_contain_text("RGB FX")
    expect(rows.nth(2)).to_contain_text("6 ch")

    page.locator("[data-pf-new]").click()
    expect(rows).to_have_count(5)
    page.locator("#pf-name").fill("Bars")
    page.locator('[data-pf-preset="dim_rgb"]').click()  # a preset keeps a name of one's own
    expect(page.locator("#pf-name")).to_have_value("Bars")
    expect(page.locator("[data-pf-slot]")).to_have_count(4)
    page.locator('[data-pf-fine="0"]').check()  # a 16-bit dimmer takes two channels
    expect(page.locator("#pf-foot")).to_have_text("5 ch")
    page.locator("[data-pf-add]").click()
    page.locator('[data-pf-fn="4"]').select_option("bank")
    page.locator('[data-pf-fn="3"]').select_option("red")
    page.locator('[data-pf-idx="3"]').select_option("1")  # the second colour's red
    page.locator('[data-pf-up="4"]').click()  # the effect channel before it
    page.locator('[data-pf-del="1"]').click()  # the first red goes
    expect(page.locator("[data-pf-ofl]")).to_have_css("pointer-events", "none")  # unsaved
    save(page)
    p = device.get("/api/config")["profiles"][4]
    assert p["name"] == "Bars" and p["footprint"] == 5
    assert [(s["fn"], s["index"], s["fine"]) for s in p["slots"]] == [
        ("dimmer", 0, True), ("green", 0, False), ("bank", 0, False), ("red", 1, False)]
    # Saved: the fixture profile downloads, named after the profile.
    with page.expect_download() as dl:
        page.locator("[data-pf-ofl]").click()
    ofl = json.loads(open(dl.value.path()).read())
    assert ofl["name"] == "pixfrog Bars" and ofl["modes"][0]["name"] == "5-channel"

    page.locator("[data-pf-remove]").click()
    save(page)
    assert len(device.get("/api/config")["profiles"]) == 4


def test_an_output_is_switched_on_and_off_not_given_an_off_protocol(page, device):
    device.post("/api/channel/1", {"protocol": "SK6812", "pixel_count": 30})
    page.reload()
    nav(page, "channels")
    expect(page.locator("#cd-proto option[value='Off']")).to_have_count(0)  # on / off is the switch
    expect(page.locator("#cd-proto option[value='WS2811']")).to_have_count(1)  # every protocol the box has
    page.locator('#chan-list-mount [data-chan="1"]').click()
    expect(page.locator("#cd-on")).to_be_checked()
    expect(page.locator("#cd-proto")).to_have_value("SK6812")
    page.locator("#cd-on").uncheck(force=True)
    expect(page.locator("#cd-row-proto")).to_be_hidden()
    expect(page.locator("#cd-fields")).to_be_hidden()
    expect(page.locator("#cd-offnote")).to_be_visible()
    save(page)
    assert device.get("/api/config")["channels"][1]["protocol"] == "Off"
    page.reload()
    nav(page, "channels")
    page.locator('#chan-list-mount [data-chan="1"]').click()
    expect(page.locator("#cd-on")).not_to_be_checked()
    page.locator("#cd-on").check(force=True)  # back on: the first protocol of the list, not the old one
    expect(page.locator("#cd-proto")).to_have_value("WS2815")
    expect(page.locator("#cd-pix")).to_have_value("30")
    save(page)
    c = device.get("/api/config")["channels"][1]
    assert c["protocol"] == "WS2815" and c["pixel_count"] == 30


def test_the_two_switches_of_an_output_pixels_and_fixtures(page, device):
    device.post("/api/channel/0", {"protocol": "WS2815", "pixel_count": 60, "universe_start": 1,
                                   "dmx_start": 1, "fixtures": [[1, 20], [21, 20], [41, 20]]})
    device.post("/api/channel/0", {"fix_universe": 1})  # an address inside the output's pixels
    page.reload()
    nav(page, "dmxpatch")
    expect(page.locator("#cd-pixmap")).to_be_checked()  # pixel mapping alone: the default
    expect(page.locator("#cd-fixctl")).not_to_be_checked()
    expect(page.locator("#cd-fdmx")).to_be_hidden()  # each switch opens its own parameters
    expect(page.locator("[data-lay-prof]")).to_have_count(0)
    expect(page.locator("#dp-mode-note")).to_be_hidden()

    # Both: the pixels keep their range, the fixtures get one of their own —
    # past the pixels, where the address they had would have sat on them.
    page.locator("#cd-fixctl").check()
    expect(page.locator("#cd-pack")).to_be_visible()
    expect(page.locator("#cd-fdmx")).to_be_visible()
    expect(page.locator("#cd-funi-flat")).to_have_text("2")
    expect(page.locator("#dp-mode-note")).to_contain_text("Effect channel is at 0")
    profs = page.locator("[data-lay-prof]")
    expect(profs).to_have_count(3)
    addr = page.locator("[data-lay-addr]")
    expect(addr.nth(0)).to_have_text("U2 · 1")
    expect(addr.nth(1)).to_have_text("U2 · 4")  # RGB: 3 channels each
    profs.nth(0).select_option("2")  # RGB FX: 6 channels — the next fixtures move on
    expect(page.locator("[data-lay-addr]").nth(1)).to_have_text("U2 · 7")
    page.locator("[data-lay-prof]").nth(2).select_option("3")  # Full: 16 channels
    expect(page.locator("[data-ctl-note]")).to_contain_text("3 fixtures · 25 ch · U2 · 1 → U2 · 25")
    page.locator("#cd-fdmx").fill("500")  # the Full fixture would straddle: it opens universe 3
    expect(page.locator("[data-lay-addr]").nth(2)).to_have_text("U3 · 1")
    expect(page.locator("[data-dp-summary]")).to_contain_text("3 universes")
    save(page)
    c = device.get("/api/config")["channels"][0]
    assert c["pixel_map"] and c["fixture_ctl"] and c["packing"] == "continuous"
    assert (c["universe_start"], c["dmx_start"]) == (1, 1)
    assert (c["fix_universe"], c["fix_dmx_start"]) == (2, 500)
    assert c["universes"] == 3  # one of pixels, two of fixtures
    assert c["fixtures"] == [[1, 20, 0, 2], [21, 20], [41, 20, 0, 3]]
    # The box computes the same addresses as the page.
    assert c["patch"] == [[2, 500, 6, 2], [2, 506, 3, 0], [3, 1, 16, 3]]
    expect(page.locator("#patch-list-mount")).to_contain_text("U1 + FX U2")
    nav(page, "patch")
    expect(page.locator("#patch-mount")).to_contain_text("pixels")
    expect(page.locator("#patch-mount")).to_contain_text("U2–U3")
    expect(page.locator("#patch-mount")).to_contain_text("3 fixtures")

    # Fixtures alone: no pixel data, and no pixel parameters.
    nav(page, "dmxpatch")
    page.locator("#cd-pixmap").uncheck()
    expect(page.locator("#cd-pack")).to_be_hidden()
    expect(page.locator("#cd-dmx")).to_be_hidden()
    expect(page.locator("#dp-mode-note")).to_be_hidden()
    expect(page.locator("[data-lay-prof]")).to_have_count(3)
    save(page)
    c = device.get("/api/config")["channels"][0]
    assert not c["pixel_map"] and c["fixture_ctl"] and c["universes"] == 2
    expect(page.locator("#patch-list-mount")).to_contain_text("FX U2")

    # Neither: the output listens to no universe.
    page.locator("#cd-fixctl").uncheck()
    expect(page.locator("#dp-mode-note")).to_contain_text("only plays scenes")
    expect(page.locator("[data-lay-prof]")).to_have_count(0)
    expect(page.locator("[data-dp-summary]")).to_contain_text("scenes only")
    save(page)
    c = device.get("/api/config")["channels"][0]
    assert not c["pixel_map"] and not c["fixture_ctl"] and c["universes"] == 0
    assert "patch" not in c
    nav(page, "patch")
    expect(page.locator("#patch-mount")).to_contain_text("scenes only")

    # Pixel mapping again: the output goes back to the layout and the address it had.
    nav(page, "dmxpatch")
    page.locator("#cd-pixmap").check()
    expect(page.locator("#cd-pack")).to_have_value("continuous")
    expect(page.locator("#cd-uni-flat")).to_have_text("1")
    save(page)
    c = device.get("/api/config")["channels"][0]
    assert c["pixel_map"] and c["packing"] == "continuous" and c["universes"] == 1


def test_auto_patch_lays_the_fixtures_block_after_the_pixels_or_at_its_own_base(page, device):
    device.post("/api/channel/0", {"protocol": "WS2815", "pixel_count": 60, "fixture_ctl": True})
    device.post("/api/channel/1", {"protocol": "WS2815", "pixel_count": 50, "pixel_map": False,
                                   "fixture_ctl": True})
    page.reload()
    nav(page, "patch")
    expect(page.locator("#ap-fixbase")).to_be_hidden()
    page.locator('[data-action="autopatch"]').click()
    expect(page.locator('[data-live="save-state"]')).to_contain_text("universes")
    chans = device.get("/api/config")["channels"]
    assert chans[0]["universe_start"] == 0 and chans[0]["fix_universe"] == 1  # pixels, then fixtures
    assert chans[1]["fix_universe"] == 2  # each output's fixtures open a universe
    page.locator("#ap-fix").select_option("base")
    expect(page.locator("#ap-fixbase")).to_be_visible()
    page.locator("#ap-fixbase").fill("40")
    page.locator('[data-action="autopatch"]').click()
    expect(page.locator("#patch-mount")).to_contain_text("U40")
    chans = device.get("/api/config")["channels"]
    assert [c["fix_universe"] for c in chans[:2]] == [40, 41]
    assert chans[0]["universe_start"] == 0
    expect(page.locator("#patch-mount")).to_contain_text("U41")


def test_a_fixture_keeps_its_profile_through_the_layout_editor(page, device):
    device.post("/api/channel/0", {"protocol": "WS2815", "pixel_count": 30,
                                   "fixtures": [[1, 10, 0, 2], [11, 10, 1, 3], [21, 10]]})
    page.reload()
    nav(page, "channels")
    page.locator("#cd-bri").fill("77")  # an edit of the channel that is not its layout
    save(page)
    c = device.get("/api/config")["channels"][0]
    assert c["brightness"] == 77
    assert c["fixtures"] == [[1, 10, 0, 2], [11, 10, 1, 3], [21, 10]]


# ── system / global ──────────────────────────────────────────────────────────

def test_speaker_volume_and_test_button(page, device):
    nav(page, "system")
    expect(page.locator("#spk-card")).to_be_visible()  # the host box reports a speaker
    expect(page.locator("#spk-vol-val")).to_have_text("Off")  # off by default
    expect(page.locator('[data-action="audio-test"]')).to_be_disabled()
    page.locator("#spk-vol").fill("60")
    page.locator("#spk-vol").dispatch_event("change")
    expect(page.locator("#spk-vol-val")).to_have_text("60%")
    expect(page.locator('[data-action="audio-test"]')).to_be_enabled()
    expect(page.locator('[data-live="save-state"]')).to_contain_text("saved")
    assert device.get("/api/config")["global"]["speaker_volume"] == 60


def test_refresh_rate_input(page, device):
    nav(page, "channels")  # with the outputs: it bounds their LED counts
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
    nav(page, "channels")
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
    page.locator('[data-ct-fn="5"]').select_option("bank")  # the effect, picked in the bank
    page.locator('[data-ct-fn="6"]').select_option("ph_wave")
    expect(page.locator('[data-ct-fn="5"] option:checked')).to_have_text("Effect of the bank")
    page.locator("#ct-en").check(force=True)
    page.locator("#ct-uni").fill("77")
    save(page)
    c = device.get("/api/config")["control"]
    assert c["enabled"] and c["universe"] == 77 and len(c["slots"]) == 15
    assert c["slots"][3] == {"fn": "scene", "mask": 15, "index": 0, "fine": False, "group": -1}
    assert c["slots"][4]["fn"] == "fseq"
    assert c["slots"][5]["fn"] == "bank" and c["slots"][6]["fn"] == "ph_wave"
    expect(page.locator("#ct-state")).to_contain_text("waiting")


def test_control_channel_aimed_at_a_group(page, device):
    device.post("/api/groups", {"groups": [{"name": "Top", "members": [[0, 0]]}]})
    page.reload()
    nav(page, "control")
    page.locator('[data-ct-preset="simple"]').click()
    page.locator('[data-ct-tgt="3"]').select_option("0")  # the Scene channel on Top
    expect(page.locator('[data-ct-out="3"]')).to_have_count(0)  # no output chips then
    page.locator("#ct-en").check(force=True)
    save(page)
    sl = device.get("/api/config")["control"]["slots"][3]
    assert sl["fn"] == "scene" and sl["group"] == 0


def test_control_overlap_and_overflow_warnings(page, device):
    nav(page, "control")
    page.locator("#ct-en").check(force=True)
    page.locator("#ct-uni").fill("1")  # channel 1's universe
    expect(page.locator("#ct-warn")).to_contain_text("also feeds channel 1")
    page.locator("#ct-addr").fill("510")  # 6 channels from 510 pass 512
    expect(page.locator("#ct-warn")).to_contain_text("past DMX channel 512")


def test_auto_patch_places_the_control_universe_after_the_outputs(page, device):
    navs = page.locator("aside div[data-nav]").evaluate_all("els => els.map(e => e.dataset.nav)")
    assert navs.index("patch") == navs.index("control") + 1  # below DMX control
    device.post("/api/control", {"enabled": True, "universe": 300, "address": 40})
    page.reload()
    nav(page, "patch")
    page.locator('[data-action="autopatch"]').click()
    expect(page.locator("#patch-mount")).to_contain_text("DMX control")
    cfg = device.get("/api/config")
    end = max(c["universe_start"] for c in cfg["channels"] if c["protocol"] != "Off")
    assert cfg["control"]["universe"] > end and cfg["control"]["address"] == 1
    expect(page.locator("#patch-mount")).to_contain_text(f'U{cfg["control"]["universe"]}')


def test_auto_patch_compact_whole_pixels_and_per_output_layout(page, device):
    nav(page, "dmxpatch")
    page.locator("#cd-pack").select_option("fixture")
    save(page)
    assert device.get("/api/config")["channels"][0]["packing"] == "fixture"
    page.locator("#cd-pack").select_option("colour")  # one colour a bar
    save(page)
    assert device.get("/api/config")["channels"][0]["packing"] == "colour"
    device.post("/api/channel/1", {"protocol": "WS2815", "pixel_count": 50})  # a second output
    page.reload()
    nav(page, "patch")
    page.locator("#ap-place").select_option("compact")
    page.locator("#ap-pack").select_option("whole")
    page.locator('[data-action="autopatch"]').click()
    expect(page.locator('[data-live="save-state"]')).to_contain_text("universes")
    chans = device.get("/api/config")["channels"]
    assert all(c["packing"] == "whole" for c in chans)
    # Compact: output 2 follows output 1 inside its universe.
    assert chans[1]["universe_start"] == chans[0]["universe_start"] + chans[0]["universes"] - 1
    assert chans[1]["dmx_start"] > 1
    expect(page.locator("#patch-pool")).to_contain_text("/ 72")
    expect(page.locator("#patch-mount")).to_contain_text("whole px")


def test_auto_patch_places_an_unsaved_control_universe(page, device):
    nav(page, "control")
    page.locator("#ct-en").check(force=True)  # enabled, not saved yet (universe 100)
    nav(page, "patch")
    page.locator('[data-action="autopatch"]').click()
    expect(page.locator('[data-live="save-state"]')).to_contain_text("universes")
    c = device.get("/api/config")["control"]
    assert c["enabled"] and c["universe"] != 100 and c["address"] == 1
    nav(page, "control")
    expect(page.locator("#ct-uni")).to_have_value(str(c["universe"]))  # no stale 100 to save back


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
    nav(page, "auto")
    page.locator("#s-fade").fill("1.5")
    save(page)
    assert device.get("/api/config")["global"]["scene_fade_ms"] == 1500


def _play_on(page, row, outputs):
    """Select scene `row`, set its part's outputs to `outputs` (0-based), press Play."""
    page.locator(f'[data-scene-row="{row}"]').click()
    n = row + 1
    for o in range(8):
        chip = page.locator(f'[data-pt-out="0"][data-o="{o}"]')
        on = "rgba(63,212,99" in (chip.get_attribute("style") or "")  # the lit chip style
        if (o in outputs) != on:
            chip.click()
    page.locator(f'[data-sc-playon="{n}"]').click()


def _scenes_on(device, want):
    for _ in range(40):
        if device.get("/api/status")["show"]["scenes"][: len(want)] == want:
            return True
        time.sleep(0.05)
    return False


def test_a_later_scene_takes_over_only_its_own_outputs(page, device):
    nav(page, "scenes")
    _play_on(page, 0, {0, 1, 2})  # scene 1 on outputs 1-3
    assert _scenes_on(device, [0, 0, 0, -1])
    _play_on(page, 1, {1})  # scene 2 on output 2 only (saved before it plays)
    assert _scenes_on(device, [0, 1, 0, -1])
    assert device.get("/api/config")["scenes"][1]["mask"] == 2
    expect(page.locator('[data-sc-playing="2"]')).to_contain_text("out 2")
    page.locator('[data-sc-stop="2"]').click()  # stops scene 2 only: output 2 back to live
    assert _scenes_on(device, [0, -1, 0])


# ── FSEQ playlist ────────────────────────────────────────────────────────────


def _playlist(device):
    return device.get("/api/fseq/playlist")


def _wait(pred):
    for _ in range(40):
        if pred():
            return True
        time.sleep(0.05)
    return pred()


def test_fseq_playlist_build_reorder_and_options(page, device):
    nav(page, "fseq")
    expect(page.locator("#pl-mount")).to_contain_text("Empty")
    page.locator('[data-fseq-add="show.fseq"]').click()
    page.locator('[data-fseq-add="loop.fseq"]').click()
    assert _wait(lambda: len(_playlist(device)["items"]) == 2)
    page.locator('[data-pl-up="1"]').click()  # loop.fseq first
    assert _wait(lambda: _playlist(device)["items"][0]["name"] == "loop.fseq")
    # The list re-renders from the save's answer: edit the new rows, not the old.
    expect(page.locator('[data-pl-row="0"]')).to_contain_text("loop.fseq")
    rep = page.locator('[data-pl-repeat="1"]')
    rep.fill("3")
    rep.dispatch_event("change")
    assert _wait(lambda: _playlist(device)["items"][1]["repeat"] == 3)
    expect(page.locator('[data-pl-repeat="1"]')).to_have_value("3")
    page.locator("#pl-loop").check()
    expect(page.locator("#pl-loop")).to_be_checked()
    assert _wait(lambda: _playlist(device)["loop"])
    nav(page, "auto")  # what plays at startup sits with the automations
    page.locator("#pl-auto").check()
    nav(page, "fseq")
    assert _wait(lambda: _playlist(device)["loop"] and _playlist(device)["autostart"])
    page.locator('[data-pl-del="0"]').click()
    assert _wait(lambda: [i["name"] for i in _playlist(device)["items"]] == ["show.fseq"])
    page.locator('[data-action="pl-play"]').click()
    expect(page.locator('[data-live="save-state"]')).to_contain_text("playlist")


def test_fseq_play_a_file_in_a_loop(page, device):
    nav(page, "fseq")
    page.locator("#fseq-loop").check()
    page.locator('[data-fseq-play="show.fseq"]').click()
    expect(page.locator('[data-live="save-state"]')).to_contain_text("show.fseq")
    assert device.get("/api/status")["fseq"]["loop"] is True


# ── live status over a WebSocket ─────────────────────────────────────────────


def test_status_arrives_over_the_websocket_and_polling_stands_down(page, device):
    frames = []
    page.on("websocket", lambda ws: ws.on(
        "framereceived", lambda f: frames.append(f) if isinstance(f, str) else None))
    page.reload()
    expect(page.locator("[data-screen-title]")).to_have_text("Dashboard")
    for _ in range(60):
        if frames:
            break
        page.wait_for_timeout(100)
    assert frames, "no status pushed over /api/ws"
    assert '"type":"status"' in frames[-1]
    polls = []
    page.on("request", lambda r: polls.append(r.url) if r.url.endswith("/api/status") else None)
    page.wait_for_timeout(3500)
    assert len(polls) <= 1, f"polling kept going while pushes arrive: {polls}"


@pytest.mark.device_args("--demo")
def test_dashboard_tiles_draw_the_pushed_output_preview(page, device):
    # --demo plays Rainbow on outputs 5-6 and renders it: their tiles' strips
    # fill with colour; output 8 is Off and has no strip.
    lit = page.locator('canvas[data-preview="4"]')
    for _ in range(50):
        if lit.evaluate("c => c.width") > 1:
            break
        page.wait_for_timeout(100)
    px = lit.evaluate("""c => {
        const d = c.getContext('2d').getImageData(0, 0, c.width, 1).data;
        let sum = 0; for (let i = 0; i < d.length; i += 4) sum += d[i] + d[i + 1] + d[i + 2];
        return {w: c.width, sum: sum};
    }""")
    assert px["w"] > 1 and px["sum"] > 0, px
    expect(page.locator('canvas[data-preview="7"]')).to_have_count(0)


def fleet_rig():
    # Three boxes that see each other through the hub's mDNS shim.
    rack_b, rack_c = Device("--mac", "30eda0000002"), Device("--mac", "30eda0000003")
    rack_b.post("/api/global", {"short_name": "Rack B"})
    rack_c.post("/api/global", {"short_name": "Rack C"})
    rack_a = Device("--mac", "30eda0000001",
                    "--peer", f"Rack B,{rack_b.port},30eda0000002,0",
                    "--peer", f"Rack C,{rack_c.port},30eda0000003,0")
    rack_a.post("/api/global", {"short_name": "Rack A"})
    return rack_a, rack_b, rack_c


def test_several_boxes_get_tabs_at_the_top_and_no_master(browser):
    rack_a, rack_b, rack_c = fleet_rig()
    ctx = browser.new_context(viewport={"width": 1440, "height": 900})
    try:
        pg = ctx.new_page()
        pg.goto(f"http://pixfrog.local:{rack_a.port}/")
        tabs = pg.locator("#pf-fleet [data-tab]")
        expect(tabs).to_have_text(["All pixfrogs", "Rack A", "Rack B", "Rack C"])
        # The bar is the first thing on the page, above any box UI.
        assert pg.evaluate("document.body.firstElementChild.id") == "pf-fleet"
        # pixfrog.local opens on the overview: one card per box, none singled out.
        cards = pg.locator("#pf-hub .hub-card")
        expect(cards).to_have_count(3)
        expect(pg.locator("#pf-app")).to_be_hidden()
        expect(cards.nth(1)).to_contain_text("pixfrog-0002.local")
        expect(cards.nth(1)).to_contain_text("fps")  # its live status arrived
        assert "this device" not in pg.locator("body").inner_text()
        expect(pg.locator("#pf-hub .hub-tag")).to_have_count(0)

        # A sibling: its own UI full page under the bar, the only sidebar shown.
        cards.nth(2).click()
        expect(tabs.nth(3)).to_have_class(re.compile(r"\bon\b"))
        ifr = pg.locator("#pf-iframe")
        expect(ifr).to_be_visible()
        assert str(rack_c.port) in ifr.get_attribute("src")
        expect(pg.locator("#pf-app")).to_be_hidden()
        expect(pg.locator("#pf-hub")).to_be_hidden()
        child = pg.frame_locator("#pf-iframe")
        expect(child.locator("aside")).to_be_visible()
        expect(child.locator("#pf-fleet")).to_have_count(0)  # no nested tab bar

        # The box that served the page is a tab like the others.
        tabs.nth(1).click()
        expect(pg.locator("#pf-app")).to_be_visible()
        expect(ifr).to_be_hidden()
        expect(pg.locator("[data-screen-title]")).to_have_text("Dashboard")
        pg.locator('div[data-nav="scenes"]').click()
        expect(tabs.nth(1)).to_have_class(re.compile(r"\bon\b"))  # the sidebar stays in its box

        # A sibling that stops answering is shown offline, not dropped.
        rack_c.close()
        tabs.nth(0).click()
        expect(cards.nth(2)).to_have_class(re.compile(r"\boff\b"), timeout=8000)

        # Opened by its own address (not pixfrog.local), a box starts on its tab.
        pg.goto(f"http://127.0.0.1:{rack_a.port}/")
        expect(tabs.first).to_be_visible()
        expect(pg.locator("#pf-fleet .pf-tab.on")).to_have_text("Rack A")
        expect(pg.locator("#pf-app")).to_be_visible()
    finally:
        ctx.close()
        rack_a.close()
        rack_b.close()
        try:
            rack_c.close()
        except Exception:
            pass


def test_a_lone_box_on_pixfrog_local_is_its_usual_ui(page, device):
    page.goto(device.url.replace("127.0.0.1", "pixfrog.local") + "/")
    expect(page.locator("[data-screen-title]")).to_have_text("Dashboard")
    expect(page.locator("#pf-hub")).to_be_hidden()
    expect(page.locator("#pf-fleet")).to_be_hidden()  # one box: no tabs
    expect(page.locator('[data-live="mdns-host"]').first).to_have_text("pixfrog-3456.local")


@pytest.mark.device_args("--no-persist")
def test_a_dead_nvs_raises_the_not_saved_banner(page, device):
    expect(page.locator("#nv-banner")).to_be_visible()
    expect(page.locator("#nv-banner")).to_contain_text("not being saved")


def test_settings_that_persist_show_no_banner(page, device):
    expect(page.locator("#rb-banner")).to_be_hidden()
    page.wait_for_timeout(1500)  # a status has been applied
    expect(page.locator("#nv-banner")).to_be_hidden()


def test_icon_colours_reach_the_svg(page, device):
    # Lucide swaps each <i data-lucide> for an <svg>, keeping its class but not
    # its style: a colour must come from a class (or a parent), never style=.
    import gzip
    with urllib.request.urlopen(device.url + "/") as r:
        html = r.read()
    html = gzip.decompress(html).decode() if html[:2] == b"\x1f\x8b" else html.decode()
    assert not re.search(r"<i\b[^<>]*data-lucide[^<>]*style=", html)
    accent, warn = "rgb(63, 212, 99)", "rgb(232, 178, 58)"
    film = page.locator('[data-screen="dashboard"] svg.fg-warn').first  # NOW PLAYING
    assert film.evaluate("e => getComputedStyle(e).color") == warn
    nav(page, "channels")
    zap = page.locator('[data-action="identify-chan"] svg').first
    assert zap.evaluate("e => getComputedStyle(e).color") == accent


# ── The GitHub Pages demo (tools/demo): the SPA against a simulated box ───────

def test_the_demo_snapshot_covers_the_api():
    # mock.js starts from tools/demo/snapshot.json: a field the API gained
    # since is missing from the demo until the snapshot is refreshed.
    r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "demo", "snapshot.py"), "--check"],
                       capture_output=True, text=True, env=dict(os.environ, PIXFROG_API_HOST=HOST_BIN))
    assert r.returncode == 0, r.stdout + r.stderr


def test_the_demo_runs_the_ui_on_a_simulated_box(browser, tmp_path):
    import functools
    import http.server
    import threading
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "demo", "build.py"),
                    str(tmp_path / "demo")], check=True, capture_output=True)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(tmp_path))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    ctx = browser.new_context(viewport={"width": 1440, "height": 900}, accept_downloads=True)
    errors = []
    try:
        pg = ctx.new_page()
        pg.on("pageerror", lambda e: errors.append(str(e)))
        pg.goto(f"http://127.0.0.1:{srv.server_port}/demo/")
        expect(pg.locator("#pf-demo-banner")).to_contain_text("simulated box")
        expect(pg.locator("[data-screen-title]")).to_have_text("Dashboard")
        expect(pg.locator('canvas[data-preview="0"]')).to_have_attribute("width", re.compile(r"[1-9]\d*"))
        for screen in ["scenes", "effects", "fseq", "channels", "groups", "profiles", "control", "network",
                       "artnet", "system", "maint", "dashboard"]:
            nav(pg, screen)
            expect(pg.locator("[data-screen-title]")).not_to_have_text("")
        # The effect bank and the scenes' parts are editable on the simulated box.
        nav(pg, "effects")
        bank = pg.locator("[data-fx-row]").count()
        pg.locator('[data-action="fx-add"]').click()
        expect(pg.locator("[data-fx-row]")).to_have_count(bank + 1)
        nav(pg, "scenes")
        pg.locator('[data-pt-fx="0"]').select_option(str(bank))
        expect(pg.locator('[data-scene-row="0"]')).to_contain_text("New effect")
        pg.locator('[data-action="save"]').first.click()
        expect(pg.locator('[data-live="save-state"]').first).to_contain_text("saved")
        # A setting sticks in the page (the simulated box keeps it).
        nav(pg, "network")
        pg.fill("#n-host", "demo-rack")
        pg.locator('[data-action="save"]').first.click()
        expect(pg.locator('[data-live="node-name"]').first).to_have_text("demo-rack")
        # Identify runs every configured output; the backup downloads.
        nav(pg, "dashboard")
        pg.locator('[data-action="identify"]').first.click()
        expect(pg.locator('[data-live="save-state"]').first).to_contain_text("Identifying outputs")
        with pg.expect_download() as dl:
            pg.locator('[data-action="export"]').first.click()
        assert json.loads(open(dl.value.path()).read())["global"]["short_name"] == "demo-rack"
    finally:
        ctx.close()
        srv.shutdown()
    assert not errors, errors
