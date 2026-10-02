// pixfrog web UI demo: a box in the browser (GitHub Pages, no hardware).
//
// Loaded before the SPA. Answers what the SPA asks a real box: fetch() and
// XMLHttpRequest on /api/..., the /api/ws WebSocket, the two downloads. The
// state starts from tools/demo/snapshot.json (the firmware's own answers,
// captured from the host build) and lives in this page: changes stick until
// a reload, nothing leaves the browser.
(function () {
  'use strict';
  var SNAP = window.PF_SNAPSHOT;
  var S = JSON.parse(JSON.stringify(SNAP));  // the box's state
  var T0 = Date.now();
  var FX_SOLID = 0;

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
    return { backup_version: 1, firmware: c.version + ' (demo)', global: clone(c.global),
             channels: clone(c.channels), scenes: clone(c.scenes), control: clone(c.control),
             playlist: clone(S.playlist) };
  }
  function activeScene() {
    var sc = S.status.show.scenes;
    for (var i = 0; i < sc.length; ++i) if (sc[i] >= 0) return sc[i];
    return -1;
  }
  function playScene(n, mask) {
    var sc = S.config.scenes[n];
    if (!sc) return bad(400, 'scene index');
    mask = mask === undefined ? sc.mask : (mask & sc.mask);  // the zone, within the scene's own
    for (var o = 0; o < 8; ++o) if (mask & (1 << o)) S.status.show.scenes[o] = n;
    S.status.active_scene = activeScene();
    return ok();
  }
  function stopScenes(pred) {
    for (var o = 0; o < 8; ++o) if (pred(S.status.show.scenes[o], o)) S.status.show.scenes[o] = -1;
    S.status.active_scene = activeScene();
  }
  var identify = { mask: 0, t0: 0 };
  function bpp(proto) { return proto === 'Off' ? 0 : (proto === 'SK6812' ? 4 : 3); }

  function route(method, path, body) {
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
      var uni = body.base || 0;
      S.config.channels.forEach(function (c) {
        c.universe_start = uni;
        var span = c.protocol === 'Off' ? 0 : Math.ceil(c.pixel_count * bpp(c.protocol) / 510);
        uni += span;
      });
      return ok({ next_free: uni });
    }
    if ((m = p.match(/^\/api\/scene\/(\d+)(\/(play|stop|delete))?$/))) {
      var n = +m[1];
      if (!S.config.scenes[n]) return bad(400, 'scene index');
      if (m[3] === 'play') return playScene(n, body.outputs);
      if (m[3] === 'stop') { stopScenes(function (s) { return s === n; }); return ok(); }
      if (m[3] === 'delete') {
        S.config.scenes.splice(n, 1);
        stopScenes(function (s) { return s === n; });
        S.status.show.scenes = S.status.show.scenes.map(function (s) { return s > n ? s - 1 : s; });
        return ok();
      }
      Object.keys(body).forEach(function (k) { S.config.scenes[n][k] = body[k]; });
      return ok();
    }
    if (p === '/api/scenes/add') {
      if (S.config.scenes.length >= 32) return bad(400, 'scene list full');
      // Optional body: the new scene's fields (a duplicate sends a whole scene).
      S.config.scenes.push(Object.assign({ name: 'Scene', effect: FX_SOLID, color: '#ffffff',
                                           colors: ['#ffffff'], speed: 0, param: 0, mask: 255 }, body));
      return ok({ index: S.config.scenes.length - 1 });
    }
    if (p === '/api/scenes/move') {
      var a = body.from, b = body.to, list = S.config.scenes;
      if (!(a in list) || !(b in list)) return bad(400, 'scene index');
      list.splice(b, 0, list.splice(a, 1)[0]);
      return ok();
    }
    if (p === '/api/scenes/stop') { stopScenes(function () { return true; }); return ok(); }
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
      ['global', 'channels', 'scenes', 'control'].forEach(function (k) {
        if (body[k]) S.config[k] = clone(body[k]);
      });
      if (body.playlist) S.playlist = clone(body.playlist);
      return ok();
    }
    if (p === '/api/factory-reset') { S = clone(SNAP); return ok({ rebooting: true }); }
    if (p === '/api/ota') return bad(400, t('demo: no firmware update on a simulated box'));
    if (p === '/api/reboot' || p === '/api/rollback/ack' || p === '/api/loglevel') return ok();
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
    var si = sh.scenes[o], sc = si >= 0 ? S.config.scenes[si] : null;
    for (k = 0; k < n; ++k) {
      var rgb;
      if (sc) {
        var cols = (sc.colors && sc.colors.length ? sc.colors : [sc.color]).map(hex);
        if (sc.effect === FX_SOLID) rgb = cols[0];
        else if (sc.effect === 2) rgb = hsv(((k / n) + now / 4000) % 1);
        else rgb = cols[Math.floor(k / 6 + now / 300) % cols.length];
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
    b.textContent = fr ? 'Démo — boîtier simulé dans votre navigateur. Rien n\'est enregistré ni envoyé ; un rechargement repart de zéro.'
                       : 'Demo — a simulated box running in your browser. Nothing is saved or sent; reloading starts over.';
    b.style.cssText = 'flex:none;padding:7px 16px;background:#e8b23a;color:#08170d;font:600 12.5px Figtree,system-ui,sans-serif;text-align:center';
    document.body.insertBefore(b, document.body.firstChild);
  });
})();
