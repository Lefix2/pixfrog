// Show control API: the DMX control universe and its OFL fixture profile,
// /api/control, /api/show, scenes.

#include "web_internal.h"

namespace pixfrog::web::impl {

// ── DMX control universe JSON ───────────────────────────────────────────────
// {"enabled":true,"universe":100,"address":1,"footprint":6,
//  "slots":[{"fn":"master","mask":255,"index":0,"fine":true}, ...]}

cJSON* build_control_json() {
    const auto& c = config::get_control();
    cJSON* jc     = cJSON_CreateObject();
    cJSON_AddBoolToObject(jc, "enabled", c.enabled);
    cJSON_AddNumberToObject(jc, "universe", c.universe);
    cJSON_AddNumberToObject(jc, "address", c.address);
    cJSON_AddNumberToObject(jc, "footprint", static_cast<double>(config::control_footprint(c)));
    // Whether the universe pool had a slot left for it (the outputs come
    // first): false = the desk is not heard. Follows the last LUT rebuild.
    cJSON_AddBoolToObject(jc, "mapped", !c.enabled || dmx::control_pool_slot() >= 0);
    cJSON_AddNumberToObject(jc, "pool", static_cast<double>(dmx::kNumUniverses));
    cJSON* js = cJSON_CreateArray();
    for (size_t i = 0; i < c.count; ++i) {
        const auto& sl = c.slots[i];
        cJSON* j       = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "fn", config::ctl_fn_id(sl.fn));
        cJSON_AddNumberToObject(j, "mask", sl.mask);
        cJSON_AddNumberToObject(j, "index", sl.index);
        cJSON_AddBoolToObject(j, "fine", (sl.flags & config::kCtlFlagFine) != 0);
        cJSON_AddItemToArray(js, j);
    }
    cJSON_AddItemToObject(jc, "slots", js);
    return jc;
}

// Applies the fields present in `jc` onto `c`. False (with *why) on anything
// out of range: nothing is half-applied by the caller then.
bool apply_control_json(const cJSON* jc, config::ControlConfig& c, const char** why) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(jc, "enabled");
    if (it) {
        if (!cJSON_IsBool(it)) return *why = "enabled: boolean", false;
        c.enabled = cJSON_IsTrue(it) ? 1 : 0;
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "universe");
    if (it) {
        if (!cJSON_IsNumber(it) || !(it->valuedouble >= 0 && it->valuedouble <= 32767))
            return *why = "universe: 0..32767", false;
        c.universe = static_cast<uint16_t>(it->valuedouble);
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "address");
    if (it) {
        if (!cJSON_IsNumber(it) || !(it->valuedouble >= 1 && it->valuedouble <= 512))
            return *why = "address: 1..512", false;
        c.address = static_cast<uint16_t>(it->valuedouble);
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "preset");
    if (it) {
        if (cJSON_IsString(it) && strcmp(it->valuestring, "simple") == 0)
            config::control_apply_preset(c, config::ControlPreset::Simple);
        else if (cJSON_IsString(it) && strcmp(it->valuestring, "full") == 0)
            config::control_apply_preset(c, config::ControlPreset::Full);
        else
            return *why = "preset: simple|full", false;
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "slots");
    if (it) {
        if (!cJSON_IsArray(it) ||
            cJSON_GetArraySize(it) > static_cast<int>(config::kMaxControlSlots))
            return *why = "slots: array of at most 32", false;
        config::ControlSlot parsed[config::kMaxControlSlots]{};
        size_t n = 0;
        for (const cJSON* js = it->child; js; js = js->next) {
            const cJSON* fn = cJSON_GetObjectItemCaseSensitive(js, "fn");
            const int f     = cJSON_IsString(fn) ? config::ctl_fn_from_id(fn->valuestring) : -1;
            if (f < 0) return *why = "slots[].fn: unknown function", false;
            config::ControlSlot sl = config::control_slot(static_cast<config::CtlFn>(f));
            const cJSON* m         = cJSON_GetObjectItemCaseSensitive(js, "mask");
            if (m) {
                if (!cJSON_IsNumber(m) || !(m->valuedouble >= 1 && m->valuedouble <= 255))
                    return *why = "slots[].mask: 1..255", false;
                sl.mask = static_cast<uint8_t>(m->valuedouble);
            }
            const cJSON* ix = cJSON_GetObjectItemCaseSensitive(js, "index");
            if (ix) {
                if (!cJSON_IsNumber(ix) ||
                    !(ix->valuedouble >= 0 && ix->valuedouble <= config::kSceneColorsMax - 1))
                    return *why = "slots[].index: 0..3", false;
                sl.index = static_cast<uint8_t>(ix->valuedouble);
            }
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(js, "fine")) &&
                f == static_cast<int>(config::CtlFn::Master))
                sl.flags = config::kCtlFlagFine;
            parsed[n++] = sl;
        }
        std::memcpy(c.slots, parsed, sizeof(c.slots));
        c.count = static_cast<uint8_t>(n);
    }
    config::ControlConfig check = c;
    config::sanitize_control(check);
    if (check.count < c.count) return *why = "the mode would end past DMX channel 512", false;
    return true;
}

// ── OFL fixture profile of the control mode ─────────────────────────────────
// Open Fixture Library format (importable in QLC+, and convertible to most
// desk formats on open-fixture-library.org): one mode, the slots in order.

static void ofl_range(cJSON* caps, int lo, int hi, cJSON* cap) {
    cJSON* r = cJSON_CreateArray();
    cJSON_AddItemToArray(r, cJSON_CreateNumber(lo));
    cJSON_AddItemToArray(r, cJSON_CreateNumber(hi));
    cJSON_AddItemToObject(cap, "dmxRange", r);
    cJSON_AddItemToArray(caps, cap);
}

static cJSON* ofl_cap(const char* type) {
    cJSON* c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", type);
    return c;
}

// "Master", or "Master (out 1-4)" when the slot does not cover every output.
static void ofl_channel_name(char* out, size_t cap, const char* base, uint8_t mask) {
    if (mask == 0xFF) {
        snprintf(out, cap, "%s", base);
        return;
    }
    char outs[24] = "";
    size_t n      = 0;
    for (int o = 0; o < 8;) {
        if (!((mask >> o) & 1)) {
            ++o;
            continue;
        }
        int e = o;
        while (e + 1 < 8 && ((mask >> (e + 1)) & 1))
            ++e;
        n += static_cast<size_t>(snprintf(outs + n, sizeof(outs) - n, "%s%d", n ? "," : "", o + 1));
        if (e > o) n += static_cast<size_t>(snprintf(outs + n, sizeof(outs) - n, "-%d", e + 1));
        o = e + 1;
    }
    snprintf(out, cap, "%s (out %s)", base, outs);
}

// "0 = the effect's own, 1-255 = override" as an OFL capability pair.
static void ofl_override(cJSON* caps, cJSON* cap) {
    ofl_range(caps, 0, 0, ofl_cap("NoFunction"));
    ofl_range(caps, 1, 255, cap);
}

static cJSON* ofl_generic(const char* comment) {
    cJSON* c = ofl_cap("Generic");
    cJSON_AddStringToObject(c, "comment", comment);
    return c;
}

// Bands of 8 over the effect bank: `none` names band 0.
static void ofl_bank(cJSON* caps, const char* none) {
    ofl_range(caps, 0, 7, ofl_generic(none));
    const size_t n = config::num_effects();
    for (size_t i = 0; i < n; ++i) {
        cJSON* c = ofl_cap("Effect");
        cJSON_AddStringToObject(c, "effectName", config::get_effect(i).name);
        ofl_range(caps, static_cast<int>(8 * (i + 1)), static_cast<int>(8 * (i + 1) + 7), c);
    }
    if (8 * (n + 1) <= 255)
        ofl_range(caps, static_cast<int>(8 * (n + 1)), 255, ofl_cap("NoFunction"));
}

// Bands of 8 over the phaser waves: band 0 = the effect's own, band 1 = none.
static void ofl_phaser_wave(cJSON* caps) {
    static const char* const kNames[] = { "No phaser",      "Phaser sine",      "Phaser cosine",
                                          "Phaser ramp up", "Phaser ramp down", "Phaser triangle",
                                          "Phaser PWM",     "Phaser bump" };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == config::kPhaserWaveCount,
                  "one name per waveform");
    ofl_range(caps, 0, 7, ofl_generic("The effect's own phaser"));
    for (int w = 0; w < config::kPhaserWaveCount; ++w) {
        cJSON* c = ofl_cap("Effect");
        cJSON_AddStringToObject(c, "effectName", kNames[w]);
        ofl_range(caps, 8 * (w + 1), 8 * (w + 1) + 7, c);
    }
    ofl_range(caps, 8 * (config::kPhaserWaveCount + 1), 255, ofl_cap("NoFunction"));
}

// "0 = the effect's own, 1 = off, 2-255 = N" (Block / Groups / Wings).
static void ofl_count(cJSON* caps, const char* what) {
    char note[48];
    ofl_range(caps, 0, 0, ofl_generic("The effect's own"));
    ofl_range(caps, 1, 1, ofl_generic("Off"));
    snprintf(note, sizeof(note), "%s: the value itself, 2 to 255", what);
    ofl_range(caps, 2, 255, ofl_generic(note));
}

static cJSON* ofl_channel(const config::ControlSlot& sl) {
    cJSON* ch     = cJSON_CreateObject();
    cJSON* caps   = cJSON_CreateArray();
    const auto fn = static_cast<config::CtlFn>(sl.fn);
    switch (fn) {
    case config::CtlFn::Master:
        cJSON_AddItemToObject(ch, "capability", ofl_cap("Intensity"));
        break;
    case config::CtlFn::Blackout: {
        ofl_range(caps, 0, 127, ofl_cap("NoFunction"));
        cJSON* c = ofl_cap("ShutterStrobe");
        cJSON_AddStringToObject(c, "shutterEffect", "Closed");
        ofl_range(caps, 128, 255, c);
        break;
    }
    case config::CtlFn::Strobe: {
        cJSON* c = ofl_cap("ShutterStrobe");
        cJSON_AddStringToObject(c, "shutterEffect", "Open");
        ofl_range(caps, 0, 0, c);
        c = ofl_cap("ShutterStrobe");
        cJSON_AddStringToObject(c, "shutterEffect", "Strobe");
        cJSON_AddStringToObject(c, "speedStart", "1Hz");
        cJSON_AddStringToObject(c, "speedEnd", "25Hz");
        ofl_range(caps, 1, 255, c);
        break;
    }
    case config::CtlFn::Scene: {
        ofl_range(caps, 0, 7, ofl_cap("NoFunction"));
        const size_t n = config::num_scenes();
        for (size_t i = 0; i < n; ++i) {
            cJSON* c = ofl_cap("Effect");
            cJSON_AddStringToObject(c, "effectName", config::get_scene(i).name);
            ofl_range(caps, static_cast<int>(8 * (i + 1)), static_cast<int>(8 * (i + 1) + 7), c);
        }
        if (8 * (n + 1) <= 255)
            ofl_range(caps, static_cast<int>(8 * (n + 1)), 255, ofl_cap("NoFunction"));
        break;
    }
    case config::CtlFn::Speed: {
        ofl_range(caps, 0, 0, ofl_cap("NoFunction"));
        cJSON* c = ofl_cap("EffectSpeed");
        cJSON_AddStringToObject(c, "speedStart", "slow");
        cJSON_AddStringToObject(c, "speedEnd", "fast");
        ofl_range(caps, 1, 255, c);
        break;
    }
    case config::CtlFn::Param: {
        ofl_range(caps, 0, 0, ofl_cap("NoFunction"));
        cJSON* c = ofl_cap("EffectParameter");
        cJSON_AddStringToObject(c, "parameterStart", "low");
        cJSON_AddStringToObject(c, "parameterEnd", "high");
        ofl_range(caps, 1, 255, c);
        break;
    }
    case config::CtlFn::Red:
    case config::CtlFn::Green:
    case config::CtlFn::Blue: {
        cJSON* c = ofl_cap("ColorIntensity");
        cJSON_AddStringToObject(c, "color",
                                fn == config::CtlFn::Red     ? "Red"
                                : fn == config::CtlFn::Green ? "Green"
                                                             : "Blue");
        cJSON_AddItemToObject(ch, "capability", c);
        break;
    }
    case config::CtlFn::Effect: {
        ofl_range(caps, 0, 0, ofl_cap("NoFunction"));
        int lo = 1;
        for (int v = 1; v <= 255; ++v) {
            const int e = dmx::effect_for_dmx_value(static_cast<uint8_t>(v));
            if (v == 255 || dmx::effect_for_dmx_value(static_cast<uint8_t>(v + 1)) != e) {
                cJSON* c = ofl_cap("Effect");
                cJSON_AddStringToObject(c, "effectName",
                                        config::scene_fx_label(static_cast<uint8_t>(e)));
                ofl_range(caps, lo, v, c);
                lo = v + 1;
            }
        }
        break;
    }
    case config::CtlFn::Bank: ofl_bank(caps, "The scene's own effect"); break;
    case config::CtlFn::PhWave: ofl_phaser_wave(caps); break;
    case config::CtlFn::PhRate: {
        cJSON* c = ofl_cap("EffectSpeed");
        cJSON_AddStringToObject(c, "speedStart", "0.05Hz");
        cJSON_AddStringToObject(c, "speedEnd", "12.75Hz");
        ofl_override(caps, c);
        break;
    }
    case config::CtlFn::PhSpread:
        ofl_override(caps, ofl_generic("Phase spread along the run, value / 16 cycles"));
        break;
    case config::CtlFn::PhWidth:
        ofl_override(caps, ofl_generic("Share of the cycle the wave takes, value / 255"));
        break;
    case config::CtlFn::Block: ofl_count(caps, "Pixels sharing a value"); break;
    case config::CtlFn::Groups: ofl_count(caps, "Pattern repeats every"); break;
    case config::CtlFn::Wings: ofl_count(caps, "Mirrored parts"); break;
    case config::CtlFn::Fade: {
        cJSON* c = ofl_cap("Generic");
        cJSON_AddStringToObject(c, "comment", "Scene fade time, value x 0.1 s");
        cJSON_AddItemToObject(ch, "capability", c);
        break;
    }
    case config::CtlFn::Fseq: {
        cJSON* c = ofl_cap("Generic");
        cJSON_AddStringToObject(c, "comment", "Stop the show file");
        ofl_range(caps, 0, 7, c);
        c = ofl_cap("Generic");
        cJSON_AddStringToObject(c, "comment", "Play show file n (8 values per file)");
        ofl_range(caps, 8, 255, c);
        break;
    }
    default: break;
    }
    if (cJSON_GetArraySize(caps) > 0)
        cJSON_AddItemToObject(ch, "capabilities", caps);
    else
        cJSON_Delete(caps);
    return ch;
}

static const char* ofl_base_name(const config::ControlSlot& sl, char* buf, size_t cap) {
    switch (static_cast<config::CtlFn>(sl.fn)) {
    case config::CtlFn::Master: return "Master";
    case config::CtlFn::Blackout: return "Blackout";
    case config::CtlFn::Strobe: return "Strobe";
    case config::CtlFn::Scene: return "Scene";
    case config::CtlFn::Speed: return "Effect speed";
    case config::CtlFn::Param: return "Effect parameter";
    case config::CtlFn::Red:
    case config::CtlFn::Green:
    case config::CtlFn::Blue:
        snprintf(buf, cap, "Colour %u %s", sl.index + 1u,
                 sl.fn == static_cast<uint8_t>(config::CtlFn::Red)     ? "red"
                 : sl.fn == static_cast<uint8_t>(config::CtlFn::Green) ? "green"
                                                                       : "blue");
        return buf;
    case config::CtlFn::Effect: return "Generator";
    case config::CtlFn::Fade: return "Fade time";
    case config::CtlFn::Fseq: return "Show file";
    case config::CtlFn::Bank: return "Effect";
    case config::CtlFn::PhWave: return "Phaser wave";
    case config::CtlFn::PhRate: return "Phaser rate";
    case config::CtlFn::PhSpread: return "Phaser spread";
    case config::CtlFn::PhWidth: return "Phaser width";
    case config::CtlFn::Block: return "Block";
    case config::CtlFn::Groups: return "Groups";
    case config::CtlFn::Wings: return "Wings";
    default: return nullptr;
    }
}

// "Sep 30 2026" (app description) → "2026-09-30".
static void iso_date(char* out, size_t cap, const char* d) {
    static const char* const kMon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mon                       = 1;
    for (int i = 0; i < 12; ++i)
        if (strncmp(d, kMon + 3 * i, 3) == 0) mon = i + 1;
    snprintf(out, cap, "%.4s-%02d-%02d", d + 7, mon, atoi(d + 4));
}

// An OFL fixture document without its channels yet.
static cJSON* ofl_fixture_root(const char* name, const char* category) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "$schema",
                            "https://raw.githubusercontent.com/OpenLightingProject/"
                            "open-fixture-library/master/schemas/fixture.json");
    cJSON_AddStringToObject(root, "name", name);
    cJSON* cats = cJSON_CreateArray();
    cJSON_AddItemToArray(cats, cJSON_CreateString(category));
    cJSON_AddItemToObject(root, "categories", cats);
    cJSON* meta    = cJSON_CreateObject();
    cJSON* authors = cJSON_CreateArray();
    cJSON_AddItemToArray(authors, cJSON_CreateString("pixfrog"));
    cJSON_AddItemToObject(meta, "authors", authors);
    char date[12];
    iso_date(date, sizeof(date), esp_app_get_description()->date);
    cJSON_AddStringToObject(meta, "createDate", date);
    cJSON_AddStringToObject(meta, "lastModifyDate", date);
    cJSON_AddItemToObject(root, "meta", meta);
    return root;
}

// Adds channel `ch` under a name unique in `avail` ("Red", then "Red 2"…) and
// lists it in the mode; `fine` adds its 16-bit fine alias right after.
static void ofl_add_channel(cJSON* avail, cJSON* mode_chs, const char* base, cJSON* ch, bool fine) {
    char unique[80];
    snprintf(unique, sizeof(unique), "%s", base);
    for (int k = 2; cJSON_GetObjectItemCaseSensitive(avail, unique); ++k)
        snprintf(unique, sizeof(unique), "%.64s %d", base, k);
    cJSON_AddItemToArray(mode_chs, cJSON_CreateString(unique));
    if (fine) {
        char alias[88];
        snprintf(alias, sizeof(alias), "%s fine", unique);
        cJSON* aliases = cJSON_CreateArray();
        cJSON_AddItemToArray(aliases, cJSON_CreateString(alias));
        cJSON_AddItemToObject(ch, "fineChannelAliases", aliases);
        cJSON_AddItemToArray(mode_chs, cJSON_CreateString(alias));
    }
    cJSON_AddItemToObject(avail, unique, ch);
}

// Closes the document with its one mode, named after the footprint.
static cJSON* ofl_fixture_finish(cJSON* root, cJSON* avail, cJSON* mode_chs, size_t footprint) {
    cJSON_AddItemToObject(root, "availableChannels", avail);
    cJSON* modes = cJSON_CreateArray();
    cJSON* mode  = cJSON_CreateObject();
    char mname[24];
    snprintf(mname, sizeof(mname), "%u-channel", static_cast<unsigned>(footprint));
    cJSON_AddStringToObject(mode, "name", mname);
    cJSON_AddItemToObject(mode, "channels", mode_chs);
    cJSON_AddItemToArray(modes, mode);
    cJSON_AddItemToObject(root, "modes", modes);
    return root;
}

static cJSON* build_fixture_json() {
    const auto& c = config::get_control();
    char name[48];
    snprintf(name, sizeof(name), "pixfrog %s control", config::get_global().short_name);
    cJSON* root     = ofl_fixture_root(name, "Other");
    cJSON* avail    = cJSON_CreateObject();
    cJSON* mode_chs = cJSON_CreateArray();
    for (size_t i = 0; i < c.count; ++i) {
        const auto& sl = c.slots[i];
        char buf[32], base[64];
        const char* b = ofl_base_name(sl, buf, sizeof(buf));
        if (!b) {  // spare channel
            cJSON_AddItemToArray(mode_chs, cJSON_CreateNull());
            continue;
        }
        ofl_channel_name(base, sizeof(base), b, sl.mask);
        ofl_add_channel(avail, mode_chs, base, ofl_channel(sl),
                        config::control_slot_width(sl) == 2);
    }
    return ofl_fixture_finish(root, avail, mode_chs, config::control_footprint(c));
}

esp_err_t handle_control_fixture(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"pixfrog-control.json\"");
    return send_json(req, build_fixture_json());
}

// ── POST /api/control ───────────────────────────────────────────────────────

esp_err_t handle_post_control(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[2048];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    config::ControlConfig c = config::get_control();
    const char* why         = nullptr;
    const bool ok           = apply_control_json(j, c, &why);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, why);
    config::set_control(c);
    dmx::mark_global_dirty();  // maps (or drops) the control universe
    return send_json(req, build_control_json());
}

// ── POST /api/show — grand master, blackout, strobe (runtime) ───────────────
// {"outputs":255, "master":80, "blackout":true|false|"toggle", "strobe_hz":5}

cJSON* build_show_json() {
    cJSON* js  = cJSON_CreateObject();
    cJSON* jm  = cJSON_CreateArray();
    cJSON* jl  = cJSON_CreateArray();
    cJSON* jst = cJSON_CreateArray();
    for (size_t o = 0; o < config::kNumChannels; ++o) {
        cJSON_AddItemToArray(
            jm,
            cJSON_CreateNumber((static_cast<unsigned>(dmx::master_effective(o)) * 1000u + 32767u) /
                               65535u / 10.0));
        cJSON_AddItemToArray(
            jl, cJSON_CreateNumber((static_cast<unsigned>(dmx::master_local(o)) * 1000u + 32767u) /
                                   65535u / 10.0));
        cJSON_AddItemToArray(jst, cJSON_CreateNumber(dmx::strobe_effective(o) / 10.0));
    }
    cJSON_AddItemToObject(js, "master", jm);
    cJSON_AddItemToObject(js, "master_local", jl);
    cJSON_AddNumberToObject(js, "blackout", dmx::blackout_effective());
    cJSON_AddNumberToObject(js, "blackout_local", dmx::blackout_local());
    cJSON_AddItemToObject(js, "strobe_hz", jst);
    cJSON_AddNumberToObject(js, "fade_ms", dmx::scene_fade_ms());
    cJSON_AddStringToObject(js, "control",
                            !config::get_control().enabled ? "off"
                            : dmx::control_live()          ? "live"
                                                           : "idle");
    cJSON* jso = cJSON_CreateArray();
    for (size_t o = 0; o < config::kNumChannels; ++o)
        cJSON_AddItemToArray(jso, cJSON_CreateNumber(dmx::scene_on_output(o)));
    cJSON_AddItemToObject(js, "scenes", jso);
    return js;
}

esp_err_t handle_post_show(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[256];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    uint8_t outs    = dmx::kAllOutputs;
    const char* why = nullptr;
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(j, "outputs");
    if (it && (!cJSON_IsNumber(it) || !(it->valuedouble >= 1 && it->valuedouble <= 255)))
        why = "outputs: 1..255";
    else if (it)
        outs = static_cast<uint8_t>(it->valuedouble);
    const cJSON* jm = cJSON_GetObjectItemCaseSensitive(j, "master");
    const cJSON* jb = cJSON_GetObjectItemCaseSensitive(j, "blackout");
    const cJSON* js = cJSON_GetObjectItemCaseSensitive(j, "strobe_hz");
    if (!why && jm && (!cJSON_IsNumber(jm) || !(jm->valuedouble >= 0 && jm->valuedouble <= 100)))
        why = "master: 0..100";
    if (!why && jb && !cJSON_IsBool(jb) &&
        !(cJSON_IsString(jb) && !strcmp(jb->valuestring, "toggle")))
        why = "blackout: true|false|\"toggle\"";
    if (!why && js && (!cJSON_IsNumber(js) || !(js->valuedouble >= 0 && js->valuedouble <= 25)))
        why = "strobe_hz: 0..25";
    if (why) {
        cJSON_Delete(j);
        return send_err(req, 400, why);
    }
    if (jm) dmx::master_set(outs, static_cast<uint16_t>(jm->valuedouble * 65535.0 / 100.0 + 0.5));
    if (jb) {
        if (cJSON_IsString(jb))
            dmx::blackout_toggle(outs);
        else
            dmx::blackout_set(outs, cJSON_IsTrue(jb));
    }
    if (js) dmx::strobe_set(outs, static_cast<uint8_t>(js->valuedouble * 10.0 + 0.5));
    cJSON_Delete(j);
    return send_json(req, build_show_json());
}

// ── Fixture DMX profiles ─────────────────────────────────────────────────────
// POST /api/profiles              {"profiles":[{name, slots:[{fn, index?, fine?}]} | {preset}]}
//                                 replaces the bank, whole or not at all
// GET  /api/profile/{n}/fixture   the profile as an Open Fixture Library fixture

cJSON* build_profiles_json() {
    const auto& bank = config::get_profiles();
    cJSON* jps       = cJSON_CreateArray();
    for (size_t i = 0; i < bank.count; ++i) {
        const auto& p = bank.profiles[i];
        cJSON* jp     = cJSON_CreateObject();
        cJSON_AddStringToObject(jp, "name", p.name);
        cJSON_AddNumberToObject(jp, "footprint", static_cast<double>(config::profile_footprint(p)));
        cJSON* jslots = cJSON_AddArrayToObject(jp, "slots");
        for (size_t k = 0; k < p.count; ++k) {
            const auto& sl = p.slots[k];
            cJSON* js      = cJSON_CreateObject();
            cJSON_AddStringToObject(js, "fn", config::fix_fn_id(sl.fn));
            cJSON_AddNumberToObject(js, "index", sl.arg & config::kProfileArgColor);
            cJSON_AddBoolToObject(js, "fine", (sl.arg & config::kProfileArgFine) != 0);
            cJSON_AddItemToArray(jslots, js);
        }
        cJSON_AddItemToArray(jps, jp);
    }
    return jps;
}

bool apply_profiles_json(const cJSON* j, config::ProfileBank& b, const char** why) {
    auto fail = [&](const char* msg) {
        *why = msg;
        return false;
    };
    const int n = cJSON_IsArray(j) ? cJSON_GetArraySize(j) : 0;
    if (n < 1 || n > static_cast<int>(config::kMaxProfiles))
        return fail("profiles: a list of 1 to 8");
    b = config::ProfileBank{};
    for (const cJSON* jp = j->child; jp; jp = jp->next) {
        config::Profile& p = b.profiles[b.count++];
        const cJSON* jpre  = cJSON_GetObjectItemCaseSensitive(jp, "preset");
        if (jpre) {
            int preset = -1;
            for (uint8_t k = 0;
                 cJSON_IsString(jpre) && k < static_cast<uint8_t>(config::ProfilePreset::Count);
                 ++k)
                if (std::strcmp(jpre->valuestring, config::profile_preset_id(
                                                       static_cast<config::ProfilePreset>(k))) == 0)
                    preset = k;
            if (preset < 0) return fail("preset: rgb, dim_rgb, rgb_fx or full");
            config::profile_apply_preset(p, static_cast<config::ProfilePreset>(preset));
        }
        const cJSON* jn = cJSON_GetObjectItemCaseSensitive(jp, "name");
        if (cJSON_IsString(jn)) {
            std::memset(p.name, 0, sizeof(p.name));
            std::strncpy(p.name, jn->valuestring, sizeof(p.name) - 1);
        }
        const cJSON* jslots = cJSON_GetObjectItemCaseSensitive(jp, "slots");
        if (!jslots) {
            if (!jpre) return fail("a profile needs slots or a preset");
            continue;
        }
        const int ns = cJSON_IsArray(jslots) ? cJSON_GetArraySize(jslots) : 0;
        if (ns < 1 || ns > static_cast<int>(config::kMaxProfileSlots))
            return fail("slots: 1 to 24 channel functions");
        p.count = 0;
        std::memset(p.slots, 0, sizeof(p.slots));
        for (const cJSON* js = jslots->child; js; js = js->next) {
            const cJSON* jf = cJSON_GetObjectItemCaseSensitive(js, "fn");
            const int fn    = cJSON_IsString(jf) ? config::fix_fn_from_id(jf->valuestring) : -1;
            if (fn < 0) return fail("slot fn: an unknown function");
            uint8_t arg     = 0;
            const cJSON* ix = cJSON_GetObjectItemCaseSensitive(js, "index");
            if (ix) {
                if (!cJSON_IsNumber(ix) ||
                    !(ix->valuedouble >= 0 && ix->valuedouble <= config::kSceneColorsMax - 1))
                    return fail("slot index: colour 0..3");
                arg = static_cast<uint8_t>(ix->valuedouble);
            }
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(js, "fine")))
                arg |= config::kProfileArgFine;
            p.slots[p.count++] = config::ProfileSlot{ static_cast<uint8_t>(fn), arg };
        }
    }
    config::sanitize_profiles(b);  // flags only on the functions they belong to
    for (size_t i = 0; i < b.count; ++i)
        if (config::profile_footprint(b.profiles[i]) > 512)
            return fail("a profile cannot pass 512 channels");
    return true;
}

esp_err_t handle_post_profiles(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char* buf = big_body_buffer();  // 8 profiles of 24 slots: ~7 kB of JSON
    if (!buf) return send_err(req, 500, "out of memory");
    if (!read_body(req, buf, kBigBodyMax - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    static config::ProfileBank bank;  // 548 B: off the httpd stack
    const char* why = nullptr;
    const bool ok   = apply_profiles_json(cJSON_GetObjectItemCaseSensitive(j, "profiles"), bank,
                                          &why);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, why);
    config::set_profiles(bank);
    dmx::mark_global_dirty();  // footprints moved: the universe map follows
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "profiles", build_profiles_json());
    return send_json(req, root);
}

static const char* ofl_profile_base(const config::ProfileSlot& sl, char* buf, size_t cap) {
    const unsigned colour = (sl.arg & config::kProfileArgColor) + 1u;
    switch (static_cast<config::FixFn>(sl.fn)) {
    case config::FixFn::Dimmer: return "Dimmer";
    case config::FixFn::Red: snprintf(buf, cap, colour > 1 ? "Red %u" : "Red", colour); return buf;
    case config::FixFn::Green:
        snprintf(buf, cap, colour > 1 ? "Green %u" : "Green", colour);
        return buf;
    case config::FixFn::Blue:
        snprintf(buf, cap, colour > 1 ? "Blue %u" : "Blue", colour);
        return buf;
    case config::FixFn::White: return "White";
    case config::FixFn::Shutter: return "Shutter";
    case config::FixFn::Bank: return "Effect";
    case config::FixFn::Speed: return "Effect speed";
    case config::FixFn::Param: return "Effect parameter";
    case config::FixFn::PhWave: return "Phaser wave";
    case config::FixFn::PhRate: return "Phaser rate";
    case config::FixFn::PhSpread: return "Phaser spread";
    case config::FixFn::PhWidth: return "Phaser width";
    case config::FixFn::Block: return "Block";
    case config::FixFn::Groups: return "Groups";
    case config::FixFn::Wings: return "Wings";
    default: return nullptr;
    }
}

// A profile slot as the control slot that maps a DMX value the same way.
static bool control_twin(const config::ProfileSlot& sl, config::ControlSlot& out) {
    config::CtlFn fn;
    switch (static_cast<config::FixFn>(sl.fn)) {
    case config::FixFn::Dimmer: fn = config::CtlFn::Master; break;
    case config::FixFn::Red: fn = config::CtlFn::Red; break;
    case config::FixFn::Green: fn = config::CtlFn::Green; break;
    case config::FixFn::Blue: fn = config::CtlFn::Blue; break;
    case config::FixFn::Shutter: fn = config::CtlFn::Strobe; break;
    case config::FixFn::Speed: fn = config::CtlFn::Speed; break;
    case config::FixFn::Param: fn = config::CtlFn::Param; break;
    case config::FixFn::PhWave: fn = config::CtlFn::PhWave; break;
    case config::FixFn::PhRate: fn = config::CtlFn::PhRate; break;
    case config::FixFn::PhSpread: fn = config::CtlFn::PhSpread; break;
    case config::FixFn::PhWidth: fn = config::CtlFn::PhWidth; break;
    case config::FixFn::Block: fn = config::CtlFn::Block; break;
    case config::FixFn::Groups: fn = config::CtlFn::Groups; break;
    case config::FixFn::Wings: fn = config::CtlFn::Wings; break;
    default: return false;
    }
    out = config::control_slot(fn);
    return true;
}

static cJSON* ofl_profile_channel(const config::ProfileSlot& sl) {
    config::ControlSlot twin;
    if (control_twin(sl, twin)) return ofl_channel(twin);
    cJSON* ch   = cJSON_CreateObject();
    cJSON* caps = cJSON_CreateArray();
    if (sl.fn == static_cast<uint8_t>(config::FixFn::White)) {
        cJSON* c = ofl_cap("ColorIntensity");
        cJSON_AddStringToObject(c, "color", "White");
        cJSON_AddItemToObject(ch, "capability", c);
        cJSON_Delete(caps);
    } else {  // Bank: band 0 is the plain colour, not "the scene's own"
        ofl_bank(caps, "No effect: colour 1, steady");
        cJSON_AddItemToObject(ch, "capabilities", caps);
    }
    return ch;
}

static cJSON* build_profile_fixture_json(const config::Profile& p) {
    char name[48];
    snprintf(name, sizeof(name), "pixfrog %s", p.name[0] ? p.name : "fixture");
    cJSON* root     = ofl_fixture_root(name, "Pixel Bar");
    cJSON* avail    = cJSON_CreateObject();
    cJSON* mode_chs = cJSON_CreateArray();
    for (size_t i = 0; i < p.count; ++i) {
        char buf[16];
        const char* base = ofl_profile_base(p.slots[i], buf, sizeof(buf));
        if (!base) {  // spare channel
            cJSON_AddItemToArray(mode_chs, cJSON_CreateNull());
            continue;
        }
        ofl_add_channel(avail, mode_chs, base, ofl_profile_channel(p.slots[i]),
                        config::profile_slot_width(p.slots[i]) == 2);
    }
    return ofl_fixture_finish(root, avail, mode_chs, config::profile_footprint(p));
}

esp_err_t handle_profile_fixture(httpd_req_t* req) {
    const char* tail = req->uri + strlen("/api/profile/");
    const int idx    = atoi(tail);
    const auto& bank = config::get_profiles();
    if (idx < 0 || idx >= bank.count || !strstr(tail, "/fixture"))
        return send_err(req, 404, "no such profile");
    static char disposition[64];  // httpd keeps the pointer until the send
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"pixfrog-profile-%d.json\"",
             idx + 1);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    return send_json(req, build_profile_fixture_json(bank.profiles[idx]));
}

// {"from":a,"to":b}, both below `count`. False (400 sent) on a bad body.
static bool read_move(httpd_req_t* req, size_t count, size_t& from, size_t& to) {
    char buf[64];
    if (!read_body(req, buf, sizeof(buf) - 1)) {
        send_err(req, 400, "body too large or empty");
        return false;
    }
    cJSON* j = cJSON_Parse(buf);
    if (!j) {
        send_err(req, 400, "invalid JSON");
        return false;
    }
    const cJSON* jf = cJSON_GetObjectItemCaseSensitive(j, "from");
    const cJSON* jt = cJSON_GetObjectItemCaseSensitive(j, "to");
    const int n     = static_cast<int>(count);
    const bool ok   = cJSON_IsNumber(jf) && cJSON_IsNumber(jt) && jf->valueint >= 0 &&
                    jf->valueint < n && jt->valueint >= 0 && jt->valueint < n;
    from = ok ? static_cast<size_t>(jf->valueint) : 0;
    to   = ok ? static_cast<size_t>(jt->valueint) : 0;
    cJSON_Delete(j);
    if (!ok) send_err(req, 400, "from/to: existing indices");
    return ok;
}

static esp_err_t send_index(httpd_req_t* req, int idx) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "index", idx);
    return send_json(req, root);
}

// An effect or a scene body (a name and 4 colours + the phaser, or a name
// and up to 8 parts). Static, off the httpd stack — the server runs one
// handler at a time.
static char g_scene_body[640];

// ── Effect bank ──────────────────────────────────────────────────────────────
// POST /api/effect/{n}          partial update (apply_effect_json)
// POST /api/effect/{n}/delete   remove it (409 while a scene plays it); later
//                               effects shift down by one, the scenes follow
// POST /api/effects/add         append (optional effect JSON body) → {"index":n}
// POST /api/effects/move        {"from":a,"to":b}

esp_err_t handle_post_effect(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;

    const char* tail = req->uri + strlen("/api/effect/");
    const int idx    = atoi(tail);
    if (idx < 0 || static_cast<size_t>(idx) >= config::num_effects())
        return send_err(req, 404, "no such effect");

    if (strstr(tail, "/delete")) {
        if (!config::delete_effect(static_cast<size_t>(idx)))
            return send_err(req, 409, "effect in use by a scene");
        return send_ok(req);
    }

    if (!read_body(req, g_scene_body, sizeof(g_scene_body) - 1))
        return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(g_scene_body);
    if (!j) return send_err(req, 400, "invalid JSON");

    config::ScopedLock lock;  // read-modify-write
    auto e = config::get_effect(static_cast<size_t>(idx));
    apply_effect_json(j, e);
    cJSON_Delete(j);
    config::set_effect(static_cast<size_t>(idx), e);
    return send_ok(req);
}

esp_err_t handle_effects_add(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    config::Effect e{};
    std::strncpy(e.name, "New effect", sizeof(e.name) - 1);
    e.num_colors = 1;
    std::memset(e.colors[0], 255, 3);
    if (req->content_len > 0) {
        if (!read_body(req, g_scene_body, sizeof(g_scene_body) - 1))
            return send_err(req, 400, "body too large");
        cJSON* j = cJSON_Parse(g_scene_body);
        if (!j) return send_err(req, 400, "invalid JSON");
        apply_effect_json(j, e);
        cJSON_Delete(j);
    }
    const int idx = config::add_effect(e);
    if (idx < 0) return send_err(req, 409, "effect bank full");
    return send_index(req, idx);
}

esp_err_t handle_effects_move(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    size_t from, to;
    if (!read_move(req, config::num_effects(), from, to)) return ESP_OK;
    config::move_effect(from, to);
    return send_ok(req);
}

// ── Scenes ───────────────────────────────────────────────────────────────────
// POST /api/scene/{n}          partial update (apply_scene_json)
// POST /api/scene/{n}/play     play it
// POST /api/scene/{n}/delete   remove it; later scenes shift down by one
// POST /api/scenes/add         append (optional scene JSON body) → {"index":n}
// POST /api/scenes/move        {"from":a,"to":b}
// POST /api/scenes/stop

esp_err_t handle_post_scene(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;

    const char* tail = req->uri + strlen("/api/scene/");
    const int idx    = atoi(tail);
    if (idx < 0 || static_cast<size_t>(idx) >= config::num_scenes())
        return send_err(req, 404, "no such scene");

    if (strstr(tail, "/play")) {
        // Optional {"outputs": mask}: the zone to claim (∩ the scene's mask).
        uint8_t outs = dmx::kAllOutputs;
        if (req->content_len > 0) {
            char pb[64];
            if (!read_body(req, pb, sizeof(pb) - 1)) return send_err(req, 400, "body too large");
            cJSON* pj       = cJSON_Parse(pb);
            const cJSON* jo = pj ? cJSON_GetObjectItemCaseSensitive(pj, "outputs") : nullptr;
            const bool bad  = !pj || (jo && (!cJSON_IsNumber(jo) ||
                                            !(jo->valuedouble >= 1 && jo->valuedouble <= 255)));
            if (!bad && jo) outs = static_cast<uint8_t>(jo->valuedouble);
            cJSON_Delete(pj);
            if (bad) return send_err(req, 400, "outputs: 1..255");
        }
        dmx::scene_start_on(static_cast<uint8_t>(idx), outs);
        return send_ok(req);
    }
    if (strstr(tail, "/stop")) {
        dmx::scene_stop_scene(static_cast<uint8_t>(idx));
        return send_ok(req);
    }
    if (strstr(tail, "/delete")) {
        config::delete_scene(static_cast<size_t>(idx));
        dmx::scene_list_edited(config::SceneEdit::Delete, static_cast<size_t>(idx));
        return send_ok(req);
    }

    if (!read_body(req, g_scene_body, sizeof(g_scene_body) - 1))
        return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(g_scene_body);
    if (!j) return send_err(req, 400, "invalid JSON");

    config::ScopedLock lock;  // read-modify-write
    auto sc         = config::get_scene(static_cast<size_t>(idx));
    const char* why = nullptr;
    const bool ok   = apply_scene_json(j, sc, &why);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, why);
    config::set_scene(static_cast<size_t>(idx), sc);
    return send_ok(req);
}

esp_err_t handle_scenes_add(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    // A new scene plays the first effect everywhere until its parts are set.
    config::Scene sc = config::make_scene("New scene", 0xFF, 0);
    if (req->content_len > 0) {
        if (!read_body(req, g_scene_body, sizeof(g_scene_body) - 1))
            return send_err(req, 400, "body too large");
        cJSON* j = cJSON_Parse(g_scene_body);
        if (!j) return send_err(req, 400, "invalid JSON");
        const char* why = nullptr;
        const bool ok   = apply_scene_json(j, sc, &why);
        cJSON_Delete(j);
        if (!ok) return send_err(req, 400, why);
    }
    const int idx = config::add_scene(sc);
    if (idx < 0) return send_err(req, 409, "scene list full");
    return send_index(req, idx);
}

esp_err_t handle_scenes_move(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    size_t from, to;
    if (!read_move(req, config::num_scenes(), from, to)) return ESP_OK;
    config::move_scene(from, to);
    dmx::scene_list_edited(config::SceneEdit::Move, from, to);
    return send_ok(req);
}

esp_err_t handle_scenes_stop(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    dmx::scene_stop();
    return send_ok(req);
}

}  // namespace pixfrog::web::impl
