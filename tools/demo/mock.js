// pixfrog web UI demo: a box in the browser (GitHub Pages, no hardware).
//
// Loaded before the SPA. Answers what the SPA asks a real box: fetch() and
// XMLHttpRequest on /api/..., the /api/ws WebSocket, the two downloads. The
// state starts from tools/demo/snapshot.json (the firmware's own answers,
// captured from the host build) and lives in this browser: every change is
// kept in localStorage, so a show prepared here survives a reload and leaves
// as a configuration or show file (Maintenance) to import on a box. Nothing
// leaves the browser. The effects are drawn by the firmware's own engine,
// compiled to WebAssembly (preview.wasm, tools/demo/preview_wasm.cpp).
(function () {
  'use strict';
  var SNAP = window.PF_SNAPSHOT;
  var STORE = 'pf-demo-box';
  var S = (function () {  // the box's state: what this browser kept, else the snapshot
    try {
      var kept = JSON.parse(localStorage.getItem(STORE) || 'null');
      if (kept && kept.config && kept.config.version === SNAP.config.version) return kept;
    } catch (e) {}
    return JSON.parse(JSON.stringify(SNAP));
  })();
  var T0 = Date.now();
  var FX_SOLID = 0;
  var saveTimer = null;
  function save() {
    try { localStorage.setItem(STORE, JSON.stringify({ config: S.config, playlist: S.playlist, fseq_files: S.fseq_files, status: S.status, logs: S.logs, fixture: S.fixture, diag: S.diag })); } catch (e) {}
  }
  function persist() {  // after a write: soon, once, and never in the way
    if (saveTimer) return;
    saveTimer = setTimeout(function () { saveTimer = null; save(); }, 300);
  }
  // A reload or a closed tab inside those 300 ms would lose the last write:
  // what is pending is written at once on the way out.
  window.addEventListener('pagehide', function () {
    if (!saveTimer) return;
    clearTimeout(saveTimer);
    saveTimer = null;
    save();
  });
  function forget() {  // and nothing pending may write it back on the way out
    if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; }
    try { localStorage.removeItem(STORE); } catch (e) {}
  }

  function clone(v) { return JSON.parse(JSON.stringify(v)); }
  function ok(extra) { return Object.assign({ ok: true }, extra || {}); }
  function bad(code, msg) { return { __status: code, __text: msg }; }

  // ── the box's routes ──────────────────────────────────────────────────────
  function configJson() {
    var c = clone(S.config);
    c.active_scene = S.status.active_scene;
    c.playlist = clone(S.playlist);
    return c;
  }
  function backupJson() {
    var c = S.config;
    return { backup_version: 2, firmware: c.version + ' (demo)', global: clone(c.global),
             channels: clone(c.channels), effects: clone(c.effects), scenes: clone(c.scenes),
             control: clone(c.control), playlist: clone(S.playlist), groups: clone(c.groups || []),
             profiles: clone(c.profiles || []) };
  }
  // A profile as the box exports it (OFL JSON, GET /api/profile/<n>/fixture):
  // the channels in order, each with its value ranges — the sketch the DMX
  // chart and the patch sheet read, after the firmware's own ladders.
  function profileFixture(pf) {
    var names = {}, avail = {}, mode = [];
    function gen(c) { return { type: 'Generic', comment: c }; }
    function range(lo, hi, cap) { cap.dmxRange = [lo, hi]; return cap; }
    function shut(effect, lo, hi, s0, s1) { var c = { type: 'ShutterStrobe', shutterEffect: effect }; if (s0) { c.speedStart = s0; c.speedEnd = s1; } return range(lo, hi, c); }
    function bank(none, colour) {
      var caps = [range(0, 0, gen(none)), range(1, 7, gen(colour))];
      S.config.effects.forEach(function (e, i) { caps.push(range(8 * (i + 1), 8 * (i + 1) + 7, { type: 'Effect', effectName: e.name })); });
      if (8 * (S.config.effects.length + 1) <= 255) caps.push(range(8 * (S.config.effects.length + 1), 255, { type: 'NoFunction' }));
      return caps;
    }
    var waves = ['the effect\'s own', 'no phaser', 'sine', 'cosine', 'ramp up', 'ramp down', 'triangle', 'PWM', 'bump'];
    var own = function (what) { return [range(0, 0, { type: 'NoFunction' }), range(1, 255, gen(what))]; };
    (pf.slots || []).forEach(function (sl) {
      var fn = sl.fn, col = (sl.index || 0) + 1, cap = null, name = null, fine = false;
      switch (fn) {
        case 'dimmer': name = 'Dimmer'; cap = { capability: { type: 'Intensity' } }; fine = !!sl.fine; break;
        case 'red': case 'green': case 'blue': name = fn[0].toUpperCase() + fn.slice(1) + (col > 1 ? ' ' + col : ''); cap = { capability: { type: 'ColorIntensity', color: name.split(' ')[0] } }; break;
        case 'white': name = 'White'; cap = { capability: { type: 'ColorIntensity', color: 'White' } }; break;
        case 'shutter': name = 'Shutter'; cap = { capabilities: [shut('Closed', 0, 31), shut('Open', 32, 63), shut('Strobe', 64, 95, '1Hz', '25Hz'), shut('Open', 96, 127), shut('Pulse', 128, 159, '0.5Hz', '10Hz'), shut('Open', 160, 191), shut('RandomStrobe', 192, 223, '1Hz', '20Hz'), shut('Open', 224, 255)] }; break;
        case 'strobe': name = 'Strobe'; cap = { capabilities: [shut('Open', 0, 9), shut('Strobe', 10, 255, '1Hz', '25Hz')] }; break;
        case 'bank': name = 'Effect'; cap = { capabilities: bank('No effect: colour 1, steady (its pixels, under pixel mapping)', 'Colour 1, steady') }; break;
        case 'speed': name = 'Speed'; cap = { capabilities: [range(0, 0, { type: 'NoFunction' }), range(1, 127, { type: 'EffectSpeed', speedStart: '1/10 of the stored tempo', speedEnd: 'the stored tempo' }), range(128, 128, gen('The stored tempo')), range(129, 255, { type: 'EffectSpeed', speedStart: 'the stored tempo', speedEnd: '10x the stored tempo' })] }; break;
        case 'param': name = 'Adjust'; cap = { capabilities: [range(0, 0, { type: 'NoFunction' }), range(1, 255, { type: 'EffectParameter', parameterStart: 'low', parameterEnd: 'high' })] }; break;
        case 'ph_wave': name = 'Phaser wave'; cap = { capabilities: waves.map(function (w, i) { return range(8 * i, 8 * i + 7, gen(w)); }).concat([range(72, 255, { type: 'NoFunction' })]) }; break;
        case 'ph_rate': name = 'Phaser rate'; cap = { capabilities: own('rate override') }; break;
        case 'ph_spread': name = 'Phaser spread'; cap = { capabilities: own('spread override') }; break;
        case 'ph_width': name = 'Phaser width'; cap = { capabilities: own('width override') }; break;
        case 'ph_attack': name = 'Phaser attack'; cap = { capabilities: [range(0, 0, { type: 'NoFunction' }), range(1, 1, gen('none (hard edge)')), range(2, 255, gen('share of the lit part'))] }; break;
        case 'ph_decay': name = 'Phaser decay'; cap = { capabilities: [range(0, 0, { type: 'NoFunction' }), range(1, 1, gen('none (hard edge)')), range(2, 255, gen('share of the lit part'))] }; break;
        case 'block': case 'groups': case 'wings': name = fn[0].toUpperCase() + fn.slice(1); cap = { capabilities: [range(0, 0, { type: 'NoFunction' }), range(1, 1, gen('off')), range(2, 255, gen('N'))] }; break;
        case 'fx_fade': name = 'Effect fade'; cap = { capabilities: [range(0, 0, { type: 'NoFunction' }), range(1, 255, gen('Effect change crossfade, value x 0.1 s'))] }; break;
        default: name = 'Spare'; cap = { capability: { type: 'NoFunction' } };
      }
      var n = name, k = 2;
      while (avail[n]) n = name + ' ' + (k++);
      if (fine) cap.fineChannelAliases = [n + ' fine'];
      avail[n] = cap; mode.push(n);
      if (fine) mode.push(n + ' fine');
    });
    return { $schema: 'https://raw.githubusercontent.com/OpenLightingProject/open-fixture-library/master/schemas/fixture.json',
             name: 'pixfrog ' + (pf.name || 'fixture'), categories: ['Pixel Bar'], meta: { authors: ['pixfrog (demo)'] },
             availableChannels: avail, modes: [{ name: mode.length + '-channel', channels: mode }] };
  }
  function activeScene() {
    var sc = S.status.show.scenes;
    for (var i = 0; i < sc.length; ++i) if (sc[i] >= 0) return sc[i];
    return -1;
  }
  // A scene's parts as the firmware keeps them: an output in one part only,
  // empty parts dropped — a scene left with none keeps its first, the look it
  // plays on a group; `mask` is every output it plays on.
  function tidyScene(sc) {
    var seen = 0;
    var all = (sc.parts || []).map(function (p) {
      var m = p.mask & ~seen;
      seen |= m;
      return { mask: m, effect: p.effect, fixture_mode: p.fixture_mode || 'each', reverse: !!p.reverse };
    });
    sc.parts = all.filter(function (p) { return p.mask; });
    if (!sc.parts.length && all.length) sc.parts = [all[0]];
    sc.mask = seen;
    if (sc.group == null) sc.group = -1;
    return sc;
  }
  function inUse(fx) {
    return S.config.scenes.some(function (sc) { return sc.parts.some(function (p) { return p.effect === fx; }); });
  }
  // The scenes' parts follow their effect through an edit of the bank.
  function remapEffects(fn) {
    S.config.scenes.forEach(function (sc) { sc.parts.forEach(function (p) { p.effect = fn(p.effect); }); });
  }
  // Scenes on fixture groups: [[scene, group], …], one scene a group.
  function plays() { return S.status.show.plays || (S.status.show.plays = []); }
  function playScene(n, mask, group) {
    var sc = S.config.scenes[n];
    if (!sc) return bad(400, 'scene index');
    if (group === undefined && mask === undefined && sc.group >= 0) group = sc.group;
    if (group !== undefined) {
      if (!(S.config.groups || [])[group]) return bad(400, 'outputs: 1..255, or group: an existing group');
      S.status.show.plays = plays().filter(function (pl) { return pl[1] !== group; }).concat([[n, group]]);
      return ok();
    }
    mask = mask === undefined ? sc.mask : (mask & sc.mask);  // the zone, within the scene's own
    for (var o = 0; o < 8; ++o) if (mask & (1 << o)) S.status.show.scenes[o] = n;
    S.status.active_scene = activeScene();
    return ok();
  }
  function stopScenes(pred) {
    for (var o = 0; o < 8; ++o) if (pred(S.status.show.scenes[o], o)) S.status.show.scenes[o] = -1;
    S.status.show.plays = plays().filter(function (pl) { return !pred(pl[0], -1); });
    S.status.active_scene = activeScene();
  }
  var identify = { mask: 0, t0: 0 };
  function bpp(proto) { return proto === 'Off' ? 0 : (proto === 'SK6812' ? 4 : 3); }

  function route(method, path, body) {
    var r = routeOnce(method, path, body);
    if (method !== 'GET') persist();
    return r;
  }
  function routeOnce(method, path, body) {
    var q = path.indexOf('?');
    var p = q >= 0 ? path.slice(0, q) : path;
    var m;
    if (method === 'GET') {
      if (p === '/api/config') return configJson();
      if (p === '/api/status') return clone(S.status);
      if (p === '/api/diag') return tickDiag();
      if (p === '/api/logs') return { __text: S.logs, __type: 'text/plain' };
      if (p === '/api/fseq/files') return clone(S.fseq_files);
      if (p === '/api/fseq/playlist') return clone(S.playlist);
      if (p === '/api/peers') return [{ name: S.config.global.short_name, ip: location.hostname, port: 80,
                                        fw: S.config.version, self: true }];
      if (p === '/api/control/fixture') return clone(S.fixture);
      if ((m = p.match(/^\/api\/profile\/(\d+)\/fixture$/))) {
        var pf = (S.config.profiles || [])[+m[1]];
        return pf ? profileFixture(pf) : bad(404, 'no such profile');
      }
      if (p === '/api/backup') return backupJson();
      if (p === '/api/coredump') return bad(404, 'no core dump');
      return bad(404, 'not found');
    }
    body = body || {};
    if (p === '/api/global') {
      var g = S.config.global;
      Object.keys(body).forEach(function (k) {
        if (k === 'web_password') g.auth_enabled = !!body[k];
        else if (k in g) g[k] = body[k];
      });
      S.status.name = g.short_name;
      return ok();
    }
    if ((m = p.match(/^\/api\/channel\/(\d)(\/identify)?$/))) {
      var ch = S.config.channels[+m[1]];
      if (!ch) return bad(400, 'channel 0..7');
      if (m[2]) { identify = { mask: 1 << +m[1], t0: Date.now() }; return ok(); }
      Object.keys(body).forEach(function (k) { if (k in ch) ch[k] = body[k]; });
      return ok();
    }
    if (p === '/api/identify') {
      var mask = body.outputs;
      if (mask === undefined) {
        mask = 0;
        S.config.channels.forEach(function (c, i) { if (c.protocol !== 'Off') mask |= 1 << i; });
      }
      identify = { mask: mask, t0: Date.now() };
      return ok({ outputs: mask });
    }
    if (p === '/api/autopatch') {
      // A sketch of the box's layout, in its blocks — the control universe,
      // one universe for the fixtures of each output under fixture control,
      // then the pixels of every output (aligned; whole pixels unless
      // continuous; compact placement is the box's job, not the demo's).
      var uni = body.base || 0, used = 0;
      if (S.config.control.enabled) { S.config.control.universe = uni++; S.config.control.address = 1; used++; }
      var fixUni = body.fix_base >= 0 ? body.fix_base : uni, own = body.fix_base >= 0;
      S.config.channels.forEach(function (c) {
        if (c.protocol === 'Off' || !c.fixture_ctl) return;
        c.fix_universe = fixUni++;
        c.fix_dmx_start = 1;
        used += 1;
      });
      if (!own) uni = fixUni;
      S.config.channels.forEach(function (c) {
        if (body.packing && body.packing !== 'keep') c.packing = body.packing;
        var b = bpp(c.protocol), span = 0;
        if (c.protocol !== 'Off' && c.pixel_map !== false) {
          c.universe_start = uni;
          c.dmx_start = 1;
          span = c.packing === 'continuous' || !c.packing ? Math.ceil(c.pixel_count * b / 512)
                                                          : Math.ceil(c.pixel_count / Math.floor(512 / b));
        } else if (c.protocol !== 'Off' && c.fixture_ctl) { c.universe_start = c.fix_universe; c.dmx_start = 1; }
        c.universes = span + (c.protocol !== 'Off' && c.fixture_ctl ? 1 : 0);
        uni += span;
        used += span;
      });
      return ok({ next_free: uni, universes: used, pool: 72 });
    }
    if (p === '/api/effect/preview') return { __bytes: previewClip(body) };
    if ((m = p.match(/^\/api\/effect\/(\d+)(\/delete)?$/))) {
      var fi = +m[1];
      if (!S.config.effects[fi]) return bad(404, 'no such effect');
      if (m[2]) {
        if (inUse(fi)) return bad(409, 'effect in use by a scene');
        S.config.effects.splice(fi, 1);
        remapEffects(function (e) { return e > fi ? e - 1 : e; });
        return ok();
      }
      Object.keys(body).forEach(function (k) { S.config.effects[fi][k] = body[k]; });
      return ok();
    }
    if (p === '/api/effects/add') {
      if (S.config.effects.length >= 31) return bad(409, 'effect bank full');
      S.config.effects.push(Object.assign({ name: 'New effect', generator: FX_SOLID, colors: ['#ffffff'],
                                            speed: 0, param: 0 }, body));
      return ok({ index: S.config.effects.length - 1 });
    }
    if (p === '/api/effects/move') {
      var fa = body.from, fb = body.to, bank = S.config.effects;
      if (!(fa in bank) || !(fb in bank)) return bad(400, 'effect index');
      bank.splice(fb, 0, bank.splice(fa, 1)[0]);
      remapEffects(function (e) {
        if (e === fa) return fb;
        if (fa < fb && e > fa && e <= fb) return e - 1;
        if (fb < fa && e >= fb && e < fa) return e + 1;
        return e;
      });
      return ok();
    }
    if ((m = p.match(/^\/api\/scene\/(\d+)(\/(play|stop|delete))?$/))) {
      var n = +m[1];
      if (!S.config.scenes[n]) return bad(400, 'scene index');
      if (m[3] === 'play') return playScene(n, body.outputs, body.group);
      if (m[3] === 'stop') { stopScenes(function (s) { return s === n; }); return ok(); }
      if (m[3] === 'delete') {
        S.config.scenes.splice(n, 1);
        stopScenes(function (s) { return s === n; });
        S.status.show.scenes = S.status.show.scenes.map(function (s) { return s > n ? s - 1 : s; });
        S.status.show.plays = plays().map(function (pl) { return [pl[0] > n ? pl[0] - 1 : pl[0], pl[1]]; });
        return ok();
      }
      Object.keys(body).forEach(function (k) { S.config.scenes[n][k] = body[k]; });
      tidyScene(S.config.scenes[n]);
      return ok();
    }
    if (p === '/api/scenes/add') {
      if (S.config.scenes.length >= 30) return bad(409, 'scene list full');
      // Optional body: the new scene's fields (a duplicate sends a whole scene).
      S.config.scenes.push(tidyScene(Object.assign(
        { name: 'New scene', group: -1, parts: [{ mask: 255, effect: 0, fixture_mode: 'each' }] }, body)));
      return ok({ index: S.config.scenes.length - 1 });
    }
    if (p === '/api/scenes/move') {
      var a = body.from, b = body.to, list = S.config.scenes;
      if (!(a in list) || !(b in list)) return bad(400, 'scene index');
      list.splice(b, 0, list.splice(a, 1)[0]);
      return ok();
    }
    if (p === '/api/scenes/stop') { stopScenes(function () { return true; }); return ok(); }
    if (p === '/api/groups') { S.config.groups = clone(body.groups || []); return ok(); }
    if (p === '/api/profiles') {
      // The footprint is the box's to compute: a 16-bit dimmer takes two channels.
      S.config.profiles = clone(body.profiles || []).map(function (pr) {
        pr.footprint = (pr.slots || []).reduce(function (a, sl) { return a + (sl.fn === 'dimmer' && sl.fine ? 2 : 1); }, 0);
        return pr;
      });
      return ok({ profiles: clone(S.config.profiles) });
    }
    if (p === '/api/show') {
      var sh = S.status.show;
      if (body.master !== undefined) sh.master = sh.master_local = sh.master.map(function () { return +body.master; });
      if (body.blackout !== undefined) {
        var on = body.blackout === 'toggle' ? !sh.blackout : !!body.blackout;
        sh.blackout = sh.blackout_local = on ? 255 : 0;
      }
      if (body.strobe_hz !== undefined) sh.strobe_hz = sh.strobe_hz.map(function () { return +body.strobe_hz; });
      return clone(sh);
    }
    if (p === '/api/control') { Object.assign(S.config.control, body); return ok(); }
    if (p === '/api/fseq/play') {
      var f = S.status.fseq;
      f.active = body.playlist ? S.playlist.items[0].name : body.filename;
      f.loop = !!body.loop; f.position_ms = 0; f.duration_ms = 60000;
      f.playlist_index = body.playlist ? 0 : -1;
      return ok();
    }
    if (p === '/api/fseq/stop') { S.status.fseq.active = ''; S.status.fseq.position_ms = 0; return ok(); }
    if (p === '/api/fseq/playlist') { S.playlist = clone(body); return ok(); }
    if (p === '/api/fseq/upload') {
      var name = decodeURIComponent((path.match(/name=([^&]+)/) || [])[1] || 'upload.fseq');
      if (S.fseq_files.files.indexOf(name) < 0) S.fseq_files.files.push(name);
      return ok();
    }
    if (p === '/api/restore') {
      ['global', 'channels', 'effects', 'scenes', 'control', 'groups', 'profiles'].forEach(function (k) {
        if (body[k]) S.config[k] = clone(body[k]);
      });
      if (body.playlist) S.playlist = clone(body.playlist);
      return ok();
    }
    if (p === '/api/factory-reset') { S = clone(SNAP); forget(); return ok({ rebooting: true }); }
    if (p === '/api/ota') return bad(400, t('demo: no firmware update on a simulated box'));
    if (p === '/api/reboot' || p === '/api/rollback/ack' || p === '/api/loglevel' ||
        p === '/api/audio/test') return ok();
    return bad(404, 'not found');
  }
  function t(s) { return S.config.global.lang === 1 ? 'démo : pas de mise à jour sur un boîtier simulé' : s; }

  // ── a living box: counters, playback, the output preview ──────────────────
  function tickStatus() {
    var st = S.status, up = Math.floor((Date.now() - T0) / 1000);
    st.uptime_s = SNAP.status.uptime_s + up;
    st.fps = S.config.global.refresh_hz - (up % 3 === 0 ? 1 : 0);
    st.artnet_rx = SNAP.status.artnet_rx + up * 250;
    st.sacn_rx = SNAP.status.sacn_rx + up * 40;
    var f = st.fseq;
    if (f.active) {
      f.position_ms += 1000;
      if (f.position_ms >= f.duration_ms) { if (f.loop || f.playlist_index >= 0) f.position_ms = 0; else f.active = ''; }
    }
    st.identify_channel = identifyChannel();
  }
  function tickDiag() {
    var d = clone(S.diag), up = Math.floor((Date.now() - T0) / 1000);
    d.render.fps = S.status.fps;
    d.render.frames_emitted = (d.render.frames_emitted || 0) + up * 60;
    d.sys.uptime_s = S.status.uptime_s;
    return d;
  }
  function identifyChannel() {
    if (!identify.mask) return -1;
    var idx = Math.floor((Date.now() - identify.t0) / 1500);
    for (var ch = 0; ch < 8; ++ch) if (identify.mask & (1 << ch)) { if (idx-- === 0) return ch; }
    identify.mask = 0;
    return -1;
  }
  function hex(c) { return [parseInt(c.slice(1, 3), 16), parseInt(c.slice(3, 5), 16), parseInt(c.slice(5, 7), 16)]; }
  function hsv(h) {
    var i = Math.floor(h * 6) % 6, f = h * 6 - Math.floor(h * 6), q = 1 - f;
    return [[1, f, 0], [q, 1, 0], [0, 1, f], [0, q, 1], [f, 0, 1], [1, 0, q]][i].map(function (x) { return x * 255; });
  }
  // The firmware's effect engine (preview.wasm, next to the page). Until it
  // has loaded, or without WebAssembly, the rough drawing below stands in.
  var W = null;
  (function loadEngine() {
    if (typeof WebAssembly === 'undefined' || typeof fetch === 'undefined') return;
    var src = (document.currentScript && document.currentScript.src) || 'mock.js';
    fetch(src.replace(/[^\/]*$/, 'preview.wasm')).then(function (r) {
      if (!r.ok) throw new Error('no engine');
      return r.arrayBuffer();
    }).then(function (buf) { return WebAssembly.instantiate(buf, {}); })
      .then(function (res) { W = res.instance.exports; })
      .catch(function () { W = null; });
  })();
  function wStr(s) {
    var bytes = new TextEncoder().encode(String(s).slice(0, 30) + '\0');
    new Uint8Array(W.memory.buffer).set(bytes, W.pf_str());
  }
  // The API's effect JSON into the engine, as apply_effect_json reads it.
  function wEffect(fx) {
    fx = fx || {};
    W.pf_effect_begin(fx.generator | 0, fx.speed | 0, fx.param | 0);
    (fx.colors || ['#ffffff']).slice(0, 4).forEach(function (c, i) { W.pf_effect_color(i, parseInt(String(c).replace('#', ''), 16) || 0); });
    var ph = fx.phaser || {}, mx = fx.matricks || {};
    var wave = 0;
    if (ph.wave) { wStr(ph.wave); wave = Math.max(0, W.pf_wave_from_id()); }
    W.pf_effect_phaser(wave, ph.rate | 0, ph.spread | 0, ph.width | 0, ph.low | 0, ph.attack | 0, ph.decay | 0, ph.reverse ? 1 : 0);
    W.pf_effect_matricks(fx.invert ? 1 : 0, mx.block | 0, mx.groups | 0, mx.wings | 0);
  }
  function wOut(n) { return new Uint8Array(W.memory.buffer, W.pf_out(), n * 3); }
  // `n` pixels of an effect at `t` ms, as the box draws them (null without the engine).
  function renderEffect(fx, n, t) {
    if (!W) return null;
    wEffect(fx);
    return wOut(W.pf_render(n, t));
  }
  // Output `o` playing effect `fx` in `mode` (each / strip / chain / mirror),
  // over its fixtures, as the box draws it.
  function renderStrip(c, fx, mode, reverse, t) {
    if (!W) return null;
    wEffect(fx);
    W.pf_strip_begin(c.pixel_count);
    (c.fixtures || []).forEach(function (f, i) { W.pf_strip_fixture(i, f[0], f[1], f[2] ? 1 : 0); });
    wStr(mode || 'each');
    var m = W.pf_fixture_mode_from_id();
    return wOut(W.pf_render_strip(m < 0 ? 0 : m, reverse ? 1 : 0, t));
  }
  window.pfDemoRender = function (fx, n, t) { var px = renderEffect(fx, n, t); return px ? Array.from(px) : null; };
  // One pixel of an effect, roughly: the stand-in while the engine loads.
  function fxPixel(fx, k, n, now) {
    var cols = (fx && fx.colors && fx.colors.length ? fx.colors : ['#000000']).map(hex);
    if (!fx || fx.generator === FX_SOLID) return cols[0];
    if (fx.generator === 2) return hsv(((k / n) + now / 4000) % 1);
    return cols[Math.floor(k / 6 + now / 300) % cols.length];
  }
  // The effect editor's preview (POST /api/effect/preview): frames × pixels RGB.
  function previewClip(body) {
    body = body || {};
    var fx = Object.assign({}, S.config.effects[body.index] || { generator: FX_SOLID, colors: ['#ffffff'] }, body.effect || {});
    var n = Math.min(144, Math.max(1, body.pixels || 60)), frames = Math.min(60, Math.max(1, body.frames || 30));
    var fps = body.fps || 30, t0 = body.t || 0, out = new Uint8Array(frames * n * 3), at = 0;
    for (var f = 0; f < frames; ++f) {
      var t = t0 + Math.floor(f * 1000 / fps), px = renderEffect(fx, n, t);
      if (px) { out.set(px, at); at += n * 3; continue; }
      for (var k = 0; k < n; ++k) {
        var rgb = fxPixel(fx, k, n, t);
        out[at++] = rgb[0]; out[at++] = rgb[1]; out[at++] = rgb[2];
      }
    }
    return out;
  }
  // What output `o` shows now, as up to 64 RGB samples (the firmware's
  // /api/ws preview): identify blink, its scene, else a moving rainbow as if a
  // desk were sending.
  function previewOf(o, now) {
    var c = S.config.channels[o];
    if (!c || c.protocol === 'Off') return [];
    var n = Math.min(64, c.pixel_count), px = [], k;
    var sh = S.status.show, dim = (sh.blackout & (1 << o)) || sh.blackout === 255 ? 0 : sh.master[o] / 100;
    if (identifyChannel() === o) {
      var lit = ((now - identify.t0) % 500) < 250;
      for (k = 0; k < n; ++k) px.push(lit ? [255, 255, 255] : [0, 0, 0]);
      return px;
    }
    // The scene's part for this output, then the effect that part plays.
    var si = sh.scenes[o], sc = si >= 0 ? S.config.scenes[si] : null;
    var part = sc ? sc.parts.filter(function (pt) { return pt.mask & (1 << o); })[0] : null;
    var fx = part ? S.config.effects[part.effect] : null;
    // The scene as the box draws it over the output's fixtures, downsampled
    // to the samples the box sends (an average a sample); the stand-in while
    // the engine loads.
    var strip = sc ? renderStrip(c, fx, part && part.fixture_mode, part && part.reverse, now - T0) : null;
    for (k = 0; k < n; ++k) {
      var rgb;
      if (strip) {
        var a = Math.floor(k * c.pixel_count / n), b = Math.floor((k + 1) * c.pixel_count / n), sum = [0, 0, 0];
        for (var q = a; q < b; ++q) { sum[0] += strip[q * 3]; sum[1] += strip[q * 3 + 1]; sum[2] += strip[q * 3 + 2]; }
        rgb = sum.map(function (x) { return x / (b - a); });
      } else if (sc) {
        rgb = fxPixel(fx, k, n, now);
      } else {
        rgb = hsv(((k / n) * 0.6 + o / 8 + now / 9000) % 1);
      }
      px.push(rgb.map(function (x) { return Math.round(x * dim); }));
    }
    return px;
  }
  function previewFrame() {
    var now = Date.now(), parts = [80, 8];
    for (var o = 0; o < 8; ++o) {
      var px = previewOf(o, now);
      parts.push(px.length);
      px.forEach(function (rgb) { parts.push(rgb[0], rgb[1], rgb[2]); });
    }
    return new Uint8Array(parts).buffer;
  }
  setInterval(tickStatus, 1000);

  // ── plumbing: fetch, XMLHttpRequest, WebSocket, downloads ─────────────────
  function apiPath(url) { var i = url.indexOf('/api/'); return url.slice(i); }
  function answer(method, url, bodyText) {
    var body;
    try { body = bodyText ? JSON.parse(bodyText) : undefined; } catch (e) { body = bodyText; }
    var r = route(method, apiPath(url), body);
    var status = r && r.__status ? r.__status : 200;
    var text = r && r.__text !== undefined ? r.__text : JSON.stringify(r);
    var type = r && r.__type ? r.__type : (r && r.__status ? 'text/plain' : 'application/json');
    if (r && r.__bytes) return { status: 200, text: r.__bytes, type: 'application/octet-stream' };
    return { status: status, text: text, type: type };
  }
  var realFetch = window.fetch.bind(window);
  window.fetch = function (input, opts) {
    var url = typeof input === 'string' ? input : input.url;
    if (url.indexOf('/api/') !== 0) return realFetch(input, opts);
    opts = opts || {};
    var a = answer((opts.method || 'GET').toUpperCase(), url, typeof opts.body === 'string' ? opts.body : '');
    return new Promise(function (res) {
      setTimeout(function () {
        res(new Response(a.text, { status: a.status, headers: { 'Content-Type': a.type } }));
      }, 40);  // a LAN's round trip
    });
  };

  var RealXHR = window.XMLHttpRequest;
  window.XMLHttpRequest = function () {
    var x = new RealXHR(), fake = null, self = this;
    this.upload = {};
    this.open = function (m, url) {
      if (url.indexOf('/api/') === 0) fake = { method: m, url: url };
      else x.open.apply(x, arguments);
    };
    this.setRequestHeader = function () { if (!fake) x.setRequestHeader.apply(x, arguments); };
    this.send = function (data) {
      if (!fake) { x.onload = self.onload; x.onerror = self.onerror; x.upload.onprogress = self.upload.onprogress; return x.send(data); }
      var total = (data && data.size) || 1, step = 0;
      var timer = setInterval(function () {
        step += 1;
        if (self.upload.onprogress) self.upload.onprogress({ lengthComputable: true, loaded: total * step / 5, total: total });
        if (step < 5) return;
        clearInterval(timer);
        var a = answer(fake.method, fake.url, '');
        self.status = a.status;
        self.responseText = a.text;
        if (self.onload) self.onload();
      }, 120);
    };
  };

  window.WebSocket = function (url) {
    var ws = this, timers = [];
    this.readyState = 0;
    this.binaryType = 'blob';
    setTimeout(function () {
      ws.readyState = 1;
      if (ws.onopen) ws.onopen({});
      timers.push(setInterval(function () {
        if (ws.onmessage) ws.onmessage({ data: previewFrame() });
      }, 200));
      timers.push(setInterval(function () {
        var st = clone(S.status);
        st.type = 'status';
        if (ws.onmessage) ws.onmessage({ data: JSON.stringify(st) });
      }, 1000));
    }, 50);
    this.send = function () {};
    this.close = function () {
      timers.forEach(clearInterval);
      ws.readyState = 3;
      if (ws.onclose) ws.onclose({});
    };
  };
  window.WebSocket.OPEN = 1;

  // Downloads: the SPA navigates (backup) or follows a link (fixture).
  function download(url) {
    var a = answer('GET', url, '');
    var name = url.indexOf('/api/backup') === 0 ? 'pixfrog-demo-backup.json' : 'pixfrog-control.json';
    var blob = new Blob([a.text], { type: a.type });
    var link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = name;
    document.body.appendChild(link);
    link.click();
    setTimeout(function () { URL.revokeObjectURL(link.href); link.remove(); }, 1000);
  }
  window.pfDemoDownload = download;
  document.addEventListener('click', function (e) {
    var a = e.target.closest && e.target.closest('a[href^="/api/"]');
    if (!a) return;
    e.preventDefault();
    download(a.getAttribute('href'));
  }, true);

  // A banner, so nobody mistakes the demo for a box.
  document.addEventListener('DOMContentLoaded', function () {
    var fr = S.config.global.lang === 1;
    var b = document.createElement('div');
    b.id = 'pf-demo-banner';
    var txt = fr ? 'Démo — boîtier simulé dans votre navigateur. Vos réglages restent dans ce navigateur, rien n\'est envoyé : préparez le show, exportez-le (Maintenance) et importez-le sur le boîtier.'
                 : 'Demo — a simulated box in your browser. Your settings stay in this browser, nothing is sent: prepare the show, export it (Maintenance) and import it on the box.';
    b.innerHTML = '<span>' + txt + '</span> <button id="pf-demo-reset" style="margin-left:10px;background:#08170d;color:#e8b23a;border:0;border-radius:0;padding:3px 9px;font:600 12px Figtree,system-ui,sans-serif;cursor:pointer">' + (fr ? 'Repartir de zéro' : 'Start over') + '</button>';
    b.style.cssText = 'flex:none;padding:7px 16px;background:#e8b23a;color:#08170d;font:600 12.5px Figtree,system-ui,sans-serif;text-align:center';
    document.body.insertBefore(b, document.body.firstChild);
    b.querySelector('#pf-demo-reset').onclick = function () {
      if (!confirm(fr ? 'Oublier les réglages gardés dans ce navigateur et repartir du boîtier de démonstration ?' : 'Forget the settings kept in this browser and start from the demo box again?')) return;
      forget(); location.reload();
    };
  });
})();
