// Configuration API: /api/config, /api/global, /api/channel, backup/restore,
// autopatch.

#include "web_internal.h"

namespace pixfrog::web::impl {

// ── GET /api/config ─────────────────────────────────────────────────────────

static cJSON* build_global_json() {
    const auto& g = config::get_global();
    cJSON* jg     = cJSON_CreateObject();
    cJSON_AddBoolToObject(jg, "dhcp", g.use_dhcp);
    cJSON_AddStringToObject(jg, "ip_fallback", config::ip_fallback_id(g.ip_fallback));

    char ip[16], mask[16], gw[16];
    fmt_ip(ip, sizeof(ip), g.static_ip);
    fmt_ip(mask, sizeof(mask), g.static_mask);
    fmt_ip(gw, sizeof(gw), g.static_gateway);
    cJSON_AddStringToObject(jg, "ip", ip);
    cJSON_AddStringToObject(jg, "mask", mask);
    cJSON_AddStringToObject(jg, "gw", gw);
    cJSON_AddNumberToObject(jg, "net", g.artnet_net);
    cJSON_AddNumberToObject(jg, "subnet", g.artnet_subnet);
    cJSON_AddStringToObject(jg, "short_name", g.short_name);
    cJSON_AddStringToObject(jg, "long_name", g.long_name);
    cJSON_AddBoolToObject(jg, "reply_unicast", g.artnet_poll_reply_unicast);
    cJSON_AddNumberToObject(jg, "refresh_hz", g.refresh_rate_hz);
    cJSON_AddNumberToObject(jg, "home_timeout_s", g.home_timeout_s);
    cJSON_AddNumberToObject(jg, "tft_brightness", config::tft_brightness_pct(g));
    cJSON_AddNumberToObject(jg, "tft_idle_dim", config::tft_idle_dim_pct(g));
    cJSON_AddNumberToObject(jg, "tft_dim_delay_s", config::tft_dim_delay_s(g));
    cJSON_AddBoolToObject(jg, "web_enabled", g.web_enabled);
    cJSON_AddBoolToObject(jg, "sacn_enabled", g.sacn_enabled);
    cJSON_AddBoolToObject(jg, "fpp_remote", g.fpp_remote);
    cJSON_AddBoolToObject(jg, "auth_enabled", config::web_password_set());
    cJSON_AddNumberToObject(jg, "failsafe_mode", g.failsafe_mode);
    cJSON_AddNumberToObject(jg, "failsafe_timeout_s", g.failsafe_timeout_s);
    cJSON_AddNumberToObject(jg, "failsafe_scene", g.failsafe_scene);
    cJSON_AddNumberToObject(jg, "boot_scene", g.boot_scene);
    cJSON_AddNumberToObject(jg, "merge_mode", g.merge_mode);
    cJSON_AddNumberToObject(jg, "lang", g.language);
    cJSON_AddBoolToObject(jg, "hub_preferred", g.hub_preferred);
    cJSON_AddNumberToObject(jg, "scene_fade_ms", g.scene_fade_ms);
    cJSON_AddNumberToObject(jg, "fseq_universe", config::fseq_universe(g));
    cJSON_AddNumberToObject(jg, "speaker_volume", config::speaker_volume_pct(g));
    char fscol[8];
    snprintf(fscol, sizeof(fscol), "#%02x%02x%02x", g.failsafe_r, g.failsafe_g, g.failsafe_b);
    cJSON_AddStringToObject(jg, "failsafe_color", fscol);
    return jg;
}

static cJSON* build_effects_json() {
    cJSON* jfx = cJSON_CreateArray();
    for (size_t i = 0; i < config::num_effects(); ++i) {
        const auto& e = config::get_effect(i);
        cJSON* je     = cJSON_CreateObject();
        cJSON_AddStringToObject(je, "name", e.name);
        cJSON_AddNumberToObject(je, "generator", e.generator);
        cJSON* jcols = cJSON_AddArrayToObject(je, "colors");
        for (size_t k = 0; k < config::effect_num_colors(e); ++k) {
            char col[8];
            snprintf(col, sizeof(col), "#%02x%02x%02x", e.colors[k][0], e.colors[k][1],
                     e.colors[k][2]);
            cJSON_AddItemToArray(jcols, cJSON_CreateString(col));
        }
        cJSON_AddNumberToObject(je, "speed", e.speed);
        cJSON_AddNumberToObject(je, "param", e.param);
        cJSON* jp = cJSON_AddObjectToObject(je, "phaser");
        cJSON_AddStringToObject(jp, "wave", config::phaser_wave_id(e.ph_wave));
        cJSON_AddNumberToObject(jp, "rate", e.ph_rate);
        cJSON_AddNumberToObject(jp, "spread", e.ph_spread);
        cJSON_AddNumberToObject(jp, "width", e.ph_width);
        cJSON_AddNumberToObject(jp, "low", e.ph_low);
        cJSON_AddNumberToObject(jp, "attack", e.ph_attack);
        cJSON_AddNumberToObject(jp, "decay", e.ph_decay);
        cJSON_AddBoolToObject(jp, "reverse", (e.flags & config::kEffectPhaserReverse) != 0);
        cJSON_AddBoolToObject(je, "invert", (e.flags & config::kEffectDimmerInvert) != 0);
        cJSON* jm = cJSON_AddObjectToObject(je, "matricks");
        cJSON_AddNumberToObject(jm, "block", e.block);
        cJSON_AddNumberToObject(jm, "groups", e.groups);
        cJSON_AddNumberToObject(jm, "wings", e.wings);
        cJSON_AddItemToArray(jfx, je);
    }
    return jfx;
}

static cJSON* build_scenes_json() {
    cJSON* jscenes = cJSON_CreateArray();
    for (size_t i = 0; i < config::num_scenes(); ++i) {
        const auto& sc = config::get_scene(i);
        cJSON* js      = cJSON_CreateObject();
        cJSON_AddStringToObject(js, "name", sc.name);
        cJSON_AddNumberToObject(js, "mask", config::scene_mask(sc));
        cJSON* jparts = cJSON_AddArrayToObject(js, "parts");
        for (size_t k = 0; k < sc.num_parts; ++k) {
            const auto& p = sc.parts[k];
            cJSON* jp     = cJSON_CreateObject();
            cJSON_AddNumberToObject(jp, "mask", p.mask);
            cJSON_AddNumberToObject(jp, "effect", p.effect);
            cJSON_AddStringToObject(jp, "fixture_mode",
                                    config::fixture_mode_id(config::scene_mode_of(p.fixture_mode)));
            cJSON_AddBoolToObject(jp, "reverse", config::scene_reverse_of(p.fixture_mode));
            cJSON_AddItemToArray(jparts, jp);
        }
        cJSON_AddNumberToObject(js, "group", config::scene_group(sc));
        cJSON_AddItemToArray(jscenes, js);
    }
    return jscenes;
}

static cJSON* build_channels_json() {
    cJSON* jchs = cJSON_CreateArray();
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        const auto& c = config::get_channel(i);
        cJSON* jc     = cJSON_CreateObject();
        cJSON_AddNumberToObject(jc, "id", static_cast<double>(i));
        cJSON_AddStringToObject(jc, "protocol", kProtoNames[static_cast<size_t>(c.protocol)]);
        cJSON_AddStringToObject(jc, "color_order", kOrderNames[static_cast<size_t>(c.color_order)]);
        cJSON_AddNumberToObject(jc, "universe_start", c.universe_start);
        cJSON_AddNumberToObject(jc, "dmx_start", c.dmx_start);
        cJSON_AddNumberToObject(jc, "pixel_count", c.pixel_count);
        cJSON_AddNumberToObject(jc, "max_pixels", dmx::channel_max_pixels(i));
        cJSON_AddNumberToObject(jc, "brightness", c.brightness);
        cJSON_AddNumberToObject(jc, "grouping", c.grouping);
        cJSON_AddBoolToObject(jc, "invert", c.invert_direction);
        cJSON_AddNumberToObject(jc, "clock_hz", static_cast<double>(c.clock_hz));
        cJSON_AddNumberToObject(jc, "gamma_x10", c.gamma_x10);
        char wb[8];
        snprintf(wb, sizeof(wb), "#%02x%02x%02x", c.wb_r, c.wb_g, c.wb_b);
        cJSON_AddStringToObject(jc, "wb", wb);
        // [[first dead LED, 1-based physical], count], ...
        cJSON* jg = cJSON_AddArrayToObject(jc, "gaps");
        for (size_t k = 0; k < led::gap_count(c.gaps, led::kMaxPixelGaps); ++k) {
            cJSON* pair = cJSON_CreateArray();
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(c.gaps[k].pos + 1));
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(c.gaps[k].len));
            cJSON_AddItemToArray(jg, pair);
        }
        // What drives the output from the network, each on its own range:
        // pixel mapping ("packing" is its layout, from universe_start /
        // dmx_start) and fixture control (the fixtures on their DMX profiles,
        // from fix_universe / fix_dmx_start).
        cJSON_AddBoolToObject(jc, "pixel_map", config::pixel_mapped(c));
        cJSON_AddStringToObject(jc, "packing", config::packing_id(config::pixel_layout(c)));
        cJSON_AddBoolToObject(jc, "fixture_ctl", config::fixture_controlled(c));
        cJSON_AddNumberToObject(jc, "fix_universe", config::fix_universe(c));
        cJSON_AddNumberToObject(jc, "fix_dmx_start", config::fix_dmx_start(c));
        cJSON_AddNumberToObject(jc, "universes",
                                static_cast<double>(dmx::channel_universe_span(c)));
        // Fixture control — the patch sheet: where each fixture sits on the
        // wire, [universe, address, channels, profile], in fixture order.
        if (config::fixture_controlled(c)) {
            dmx::FixtureAddress at[config::kMaxFixtures];
            const size_t n = dmx::fixture_patch(c, at, config::kMaxFixtures);
            cJSON* jpatch  = cJSON_AddArrayToObject(jc, "patch");
            for (size_t k = 0; k < n; ++k) {
                cJSON* row = cJSON_CreateArray();
                cJSON_AddItemToArray(row, cJSON_CreateNumber(at[k].universe));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(at[k].address));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(at[k].footprint));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(at[k].profile));
                cJSON_AddItemToArray(jpatch, row);
            }
        }
        // [[first LED, 1-based physical], count], ... — same shape as the gaps.
        cJSON* jf = cJSON_AddArrayToObject(jc, "fixtures");
        for (size_t k = 0; k < config::fixture_count(c.fixtures, config::kMaxFixtures); ++k) {
            cJSON* pair = cJSON_CreateArray();
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(c.fixtures[k].pos + 1));
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(config::fixture_len(c.fixtures[k])));
            const uint8_t profile = config::fixture_profile(c.fixtures[k]);
            // [first, count, reversed, profile]: the last two only when they say something
            if (profile) {
                cJSON_AddItemToArray(
                    pair, cJSON_CreateNumber(config::fixture_reversed(c.fixtures[k]) ? 1 : 0));
                cJSON_AddItemToArray(pair, cJSON_CreateNumber(profile));
            } else if (config::fixture_reversed(
                           c.fixtures[k]))  // [first, count, 1]: mounted backwards
                cJSON_AddItemToArray(pair, cJSON_CreateNumber(1));
            cJSON_AddItemToArray(jf, pair);
        }
        cJSON_AddItemToArray(jchs, jc);
    }
    return jchs;
}

// "gaps": [[first dead LED (1-based), count], ...] replaces the whole list when
// present and well-formed (≤ kMaxPixelGaps pairs, in range); config_store
// normalizes (sort + merge) on write.
static void apply_gaps_json(const cJSON* jc, config::ChannelConfig& c) {
    const cJSON* jg = cJSON_GetObjectItemCaseSensitive(jc, "gaps");
    if (!cJSON_IsArray(jg) || cJSON_GetArraySize(jg) > static_cast<int>(led::kMaxPixelGaps)) return;
    led::PixelGap parsed[led::kMaxPixelGaps] = {};
    size_t n                                 = 0;
    for (const cJSON* pair = jg->child; pair; pair = pair->next) {
        const cJSON* jp = cJSON_GetArrayItem(pair, 0);
        const cJSON* jl = cJSON_GetArrayItem(pair, 1);
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jl) || jp->valuedouble < 1 ||
            jp->valuedouble > led::kMaxPixelsPerChannel || jl->valuedouble < 0 ||
            jl->valuedouble > led::kMaxPixelsPerChannel)
            return;
        parsed[n].pos   = static_cast<uint16_t>(jp->valuedouble - 1);
        parsed[n++].len = static_cast<uint16_t>(jl->valuedouble);
    }
    std::memcpy(c.gaps, parsed, sizeof(c.gaps));
}

// ── GET /api/config ─────────────────────────────────────────────────────────

// The patch's overlaps (dmx::patch_clashes): [{"a": {"output", "range"},
// "b": …, "universe", "address", "channels"}], one per pair of ranges that
// share DMX channels. "range" is pixels, fixtures or control (output -1).
static cJSON* patch_owner_json(uint8_t owner) {
    cJSON* j = cJSON_CreateObject();
    if (owner >= dmx::kPatchControl) {
        cJSON_AddNumberToObject(j, "output", -1);
        cJSON_AddStringToObject(j, "range", "control");
    } else {
        cJSON_AddNumberToObject(j, "output", owner % config::kNumChannels);
        cJSON_AddStringToObject(j, "range", owner < config::kNumChannels ? "pixels" : "fixtures");
    }
    return j;
}

static cJSON* build_patch_clashes_json() {
    static dmx::PatchClash clashes[dmx::kMaxPatchClashes];  // off the httpd stack
    const size_t n = dmx::patch_clashes(clashes, dmx::kMaxPatchClashes);
    cJSON* arr     = cJSON_CreateArray();
    for (size_t i = 0; i < n; ++i) {
        cJSON* j = cJSON_CreateObject();
        cJSON_AddItemToObject(j, "a", patch_owner_json(clashes[i].a));
        cJSON_AddItemToObject(j, "b", patch_owner_json(clashes[i].b));
        cJSON_AddNumberToObject(j, "universe", clashes[i].universe);
        cJSON_AddNumberToObject(j, "address", clashes[i].address);
        cJSON_AddNumberToObject(j, "channels", clashes[i].channels);
        cJSON_AddItemToArray(arr, j);
    }
    return arr;
}

esp_err_t handle_get_config(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "link", ui::is_link_up());
    cJSON_AddNumberToObject(root, "fps", static_cast<double>(dmx::get_stats().current_fps));
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "ota_partition", esp_ota_get_running_partition()->label);
    cJSON_AddNumberToObject(root, "active_scene", dmx::active_scene());
    cJSON_AddNumberToObject(root, "identify_channel", dmx::identify_channel());
    const char* fseq_file = fseq::active_file();
    cJSON_AddStringToObject(root, "active_fseq", fseq_file ? fseq_file : "");
    cJSON_AddItemToObject(root, "global", build_global_json());
    cJSON_AddItemToObject(root, "effects", build_effects_json());
    cJSON_AddItemToObject(root, "scenes", build_scenes_json());
    cJSON_AddItemToObject(root, "channels", build_channels_json());
    cJSON_AddItemToObject(root, "control", build_control_json());
    cJSON_AddItemToObject(root, "playlist", build_playlist_json());
    cJSON_AddItemToObject(root, "groups", build_groups_json());
    cJSON_AddItemToObject(root, "profiles", build_profiles_json());
    cJSON_AddItemToObject(root, "patch_clashes", build_patch_clashes_json());
    return send_json(req, root);
}

// ── GET /api/backup + POST /api/restore ─────────────────────────────────────
// Backup = the persisted configuration only (no live status, no password
// hash). Restore applies best-effort: unknown or invalid fields are skipped
// so a backup from a different firmware degrades gracefully.

// pixfrog-<short name>[-<date>].json, so the backups of several boxes do not
// collide. The box has no clock: the SPA passes ?date=YYYY-MM-DD from the
// browser; anything but digits and dashes is ignored.
static void backup_filename(httpd_req_t* req, char* out, size_t cap) {
    char slug[24] = "";
    size_t n      = 0;
    bool dash     = false;
    for (const char* p = config::get_global().short_name; *p && n + 1 < sizeof(slug); ++p) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && n) slug[n++] = '-';
            if (n + 1 < sizeof(slug)) slug[n++] = c;
            dash = false;
        } else {
            dash = true;  // runs of anything else become one dash
        }
    }
    slug[n] = '\0';
    if (std::strcmp(slug, "pixfrog") == 0) slug[0] = '\0';  // the default: no pixfrog-pixfrog
    char query[48] = "", date[12] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "date", date, sizeof(date)) == ESP_OK) {
        for (const char* p = date; *p; ++p)
            if (!std::isdigit(static_cast<unsigned char>(*p)) && *p != '-') date[0] = '\0';
    }
    std::snprintf(out, cap, "attachment; filename=\"pixfrog%s%s%s%s.json\"", slug[0] ? "-" : "",
                  slug, date[0] ? "-" : "", date);
}

esp_err_t handle_backup(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "backup_version", 2);  // 2: effect bank, scenes of parts
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddItemToObject(root, "global", build_global_json());
    cJSON_AddItemToObject(root, "channels", build_channels_json());
    cJSON_AddItemToObject(root, "effects", build_effects_json());
    cJSON_AddItemToObject(root, "scenes", build_scenes_json());
    cJSON_AddItemToObject(root, "control", build_control_json());
    cJSON_AddItemToObject(root, "playlist", build_playlist_json());
    cJSON_AddItemToObject(root, "groups", build_groups_json());
    cJSON_AddItemToObject(root, "profiles", build_profiles_json());
    static char disposition[96];  // httpd keeps the pointer until the send
    backup_filename(req, disposition, sizeof(disposition));
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    return send_json(req, root);
}

// ── Field parsers shared by POST /api/global|channel and restore ──────────
// One parser per object, so a field's bounds are written once. Every valid
// field is applied; an invalid number, bool or colour is skipped. The first
// field a POST must refuse (an address, an enum name) is reported in *why:
// the handler answers 400 and saves nothing, restore keeps the rest.

namespace {

const cJSON* field(const cJSON* j, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(j, key);
}

bool json_u32(const cJSON* j, const char* key, uint32_t lo, uint32_t hi, uint32_t& out) {
    const cJSON* it = field(j, key);
    if (!cJSON_IsNumber(it)) return false;
    // Range-check the double first: casting an out-of-range value (-5, 1e40)
    // to uint32_t is undefined (RISC-V saturates -5 to 0).
    const double d = it->valuedouble;
    if (!(d >= lo && d <= hi)) return false;
    out = static_cast<uint32_t>(d);
    return true;
}

bool json_bool(const cJSON* j, const char* key, bool& out) {  // true/false or 0/1
    const cJSON* it = field(j, key);
    if (cJSON_IsBool(it)) {
        out = cJSON_IsTrue(it);
        return true;
    }
    if (cJSON_IsNumber(it)) {
        out = it->valuedouble != 0;
        return true;
    }
    return false;
}

const char* json_str(const cJSON* j, const char* key) {
    const cJSON* it = field(j, key);
    return cJSON_IsString(it) ? it->valuestring : nullptr;
}

// "#rrggbb" or "rrggbb".
bool hex_rgb(const char* s, unsigned& r, unsigned& g, unsigned& b) {
    return s && sscanf(s[0] == '#' ? s + 1 : s, "%02x%02x%02x", &r, &g, &b) == 3;
}

void copy_name(char* dst, size_t cap, const char* src) {
    memset(dst, 0, cap);
    strncpy(dst, src, cap - 1);
}

void refuse(const char** why, const char* msg) {
    if (why && !*why) *why = msg;
}

// "fixtures": [[first LED (1-based), count], ...] replaces the whole list. A
// malformed list, or two fixtures sharing an LED, is refused and the stored
// list kept: fixtures never overlap (a dead LED inside one is fine).
void apply_fixtures_json(const cJSON* jc, config::ChannelConfig& c, const char** why) {
    const cJSON* jf = cJSON_GetObjectItemCaseSensitive(jc, "fixtures");
    if (!jf) return;
    if (!cJSON_IsArray(jf) || cJSON_GetArraySize(jf) > static_cast<int>(config::kMaxFixtures))
        return refuse(why, "fixtures: at most 32 [first, count] pairs");
    config::Fixture parsed[config::kMaxFixtures] = {};
    size_t n                                     = 0;
    for (const cJSON* pair = jf->child; pair; pair = pair->next) {
        const cJSON* jp = cJSON_GetArrayItem(pair, 0);
        const cJSON* jl = cJSON_GetArrayItem(pair, 1);
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jl) || jp->valuedouble < 1 ||
            jl->valuedouble < 1 ||
            jp->valuedouble + jl->valuedouble - 1 > led::kMaxPixelsPerChannel)
            return refuse(why, "fixtures: first 1..1024, count 1.., within 1024 LEDs");
        const cJSON* jr = cJSON_GetArrayItem(pair, 2);  // optional third: 1 = reversed
        const bool rev  = cJSON_IsNumber(jr) && jr->valuedouble != 0;
        const cJSON* jq = cJSON_GetArrayItem(pair, 3);  // optional fourth: the DMX profile
        if (jq && (!cJSON_IsNumber(jq) ||
                   !(jq->valuedouble >= 0 && jq->valuedouble <= config::kMaxProfiles - 1)))
            return refuse(why, "fixtures: profile 0..7");
        parsed[n++] = config::make_fixture(static_cast<uint16_t>(jp->valuedouble - 1),
                                           static_cast<uint16_t>(jl->valuedouble), rev,
                                           jq ? static_cast<uint8_t>(jq->valuedouble) : 0);
    }
    for (size_t a = 0; a < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
            if (parsed[a].pos < parsed[b].pos + config::fixture_len(parsed[b]) &&
                parsed[b].pos < parsed[a].pos + config::fixture_len(parsed[a])) {
                static char msg[48];
                snprintf(msg, sizeof(msg), "fixtures %u and %u overlap",
                         static_cast<unsigned>(a + 1), static_cast<unsigned>(b + 1));
                return refuse(why, msg);
            }
    std::memcpy(c.fixtures, parsed, sizeof(c.fixtures));
}

}  // namespace

GlobalApplied apply_global_json(const cJSON* j, config::GlobalConfig& g, const char** why) {
    GlobalApplied fx;
    uint32_t u = 0;
    bool b     = false;
    const char* s;
    // The addressing changed only when a value did: a page that saves its
    // whole network group every time must not re-address the box every time.
    if (json_bool(j, "dhcp", b)) {
        if (g.use_dhcp != b) fx.network = true;
        g.use_dhcp = b;
    }
    struct {
        const char* key;
        uint32_t* dst;
        const char* msg;
    } const addrs[] = { { "ip", &g.static_ip, "bad ip" },
                        { "mask", &g.static_mask, "bad mask" },
                        { "gw", &g.static_gateway, "bad gw" } };
    for (const auto& a : addrs) {
        if (!(s = json_str(j, a.key))) continue;
        uint32_t ip;
        if (!parse_ip(s, ip)) {
            refuse(why, a.msg);
            continue;
        }
        if (*a.dst != ip) fx.network = true;
        *a.dst = ip;
    }
    // Read when the fallback is taken: no reboot needed.
    if ((s = json_str(j, "ip_fallback"))) {
        const int fb = config::ip_fallback_from_id(s);
        if (fb < 0)
            refuse(why, "ip_fallback: linklocal|artnet");
        else
            g.ip_fallback = static_cast<uint8_t>(fb);
    }
    if (json_u32(j, "net", 0, 127, u)) g.artnet_net = static_cast<uint8_t>(u);
    if (json_u32(j, "subnet", 0, 15, u)) g.artnet_subnet = static_cast<uint8_t>(u);
    if ((s = json_str(j, "short_name"))) copy_name(g.short_name, sizeof(g.short_name), s);
    if ((s = json_str(j, "long_name"))) copy_name(g.long_name, sizeof(g.long_name), s);
    if (json_bool(j, "reply_unicast", b)) g.artnet_poll_reply_unicast = b;
    if (json_u32(j, "refresh_hz", config::kMinRefreshHz, config::kMaxRefreshHz, u))
        g.refresh_rate_hz = static_cast<uint8_t>(u);
    if (json_u32(j, "home_timeout_s", 0, 65535, u)) g.home_timeout_s = static_cast<uint16_t>(u);
    if (json_u32(j, "tft_brightness", config::kTftBrightnessMin, 100, u))
        g.tft_brightness = static_cast<uint8_t>(u);
    if (json_u32(j, "tft_idle_dim", 0, 100, u)) g.tft_idle_dim = static_cast<uint8_t>(u);
    if (json_u32(j, "tft_dim_delay_s", 0, config::kTftDimDelayMaxS, u))
        g.tft_dim_delay_s = static_cast<uint16_t>(u);
    if (json_bool(j, "web_enabled", b)) {
        g.web_enabled = b;
        fx.web_off    = !b;
    }
    if (json_bool(j, "sacn_enabled", b)) {
        fx.sacn        = g.sacn_enabled != b;
        g.sacn_enabled = b;
    }
    if (json_bool(j, "fpp_remote", b)) {
        fx.fpp       = g.fpp_remote != b;
        g.fpp_remote = b;
    }
    if (json_u32(j, "failsafe_mode", 0, 3, u)) g.failsafe_mode = static_cast<uint8_t>(u);
    if (json_u32(j, "failsafe_timeout_s", 0, 3600, u))
        g.failsafe_timeout_s = static_cast<uint16_t>(u);
    unsigned r, gr, bl;
    if (hex_rgb(json_str(j, "failsafe_color"), r, gr, bl)) {
        g.failsafe_r = static_cast<uint8_t>(r);
        g.failsafe_g = static_cast<uint8_t>(gr);
        g.failsafe_b = static_cast<uint8_t>(bl);
    }
    // Bounded by the list capacity, not its current length: a reference past
    // the end plays nothing (scene_start / get_scene bound-check at use).
    if (json_u32(j, "failsafe_scene", 0, config::kMaxScenes - 1, u))
        g.failsafe_scene = static_cast<uint8_t>(u);
    if (json_u32(j, "boot_scene", 0, config::kMaxScenes, u)) g.boot_scene = static_cast<uint8_t>(u);
    if (json_u32(j, "merge_mode", 0, 1, u)) g.merge_mode = static_cast<uint8_t>(u);
    if (json_u32(j, "lang", 0, 1, u)) g.language = static_cast<uint8_t>(u);
    if (json_bool(j, "hub_preferred", b)) g.hub_preferred = b;
    if (json_u32(j, "scene_fade_ms", 0, config::kMaxSceneFadeMs, u))
        g.scene_fade_ms = static_cast<uint16_t>(u);
    if (json_u32(j, "fseq_universe", 1, dmx::kMaxUniverseNumber, u))
        g.fseq_universe = static_cast<uint16_t>(u);
    if (json_u32(j, "speaker_volume", 0, 100, u)) g.speaker_volume = static_cast<uint8_t>(u);
    return fx;
}

void apply_channel_json(const cJSON* j, config::ChannelConfig& c, const char** why) {
    uint32_t u = 0;
    bool b     = false;
    const char* s;
    if ((s = json_str(j, "protocol"))) {
        const int p = lookup(kProtoNames, static_cast<size_t>(led::Protocol::COUNT), s);
        if (p >= 0) {
            c.protocol = static_cast<led::Protocol>(p);
        } else {
            // DMX512 output moved to the DMX node firmware: a backup from that
            // era comes back disabled; a POST asking for it is refused.
            if (std::strcmp(s, "DMX512") == 0) c.protocol = led::Protocol::Off;
            refuse(why, "bad protocol");
        }
    }
    if ((s = json_str(j, "color_order"))) {
        const int o = lookup(kOrderNames, static_cast<size_t>(led::ColorOrder::COUNT), s);
        if (o >= 0)
            c.color_order = static_cast<led::ColorOrder>(o);
        else
            refuse(why, "bad color_order");
    }
    if (json_u32(j, "universe_start", 0, 32767, u)) c.universe_start = static_cast<uint16_t>(u);
    if (json_u32(j, "dmx_start", 1, 512, u)) c.dmx_start = static_cast<uint16_t>(u);
    if (json_u32(j, "pixel_count", 1, dmx::kMaxPixelsPerChan, u))
        c.pixel_count = static_cast<uint16_t>(u);
    if (json_u32(j, "brightness", 0, 255, u)) c.brightness = static_cast<uint8_t>(u);
    if (json_u32(j, "grouping", 1, 8, u)) c.grouping = static_cast<uint8_t>(u);
    if (json_bool(j, "invert", b)) c.invert_direction = b;
    if (json_u32(j, "clock_hz", led::kMinClockHz, led::kMaxClockHz, u)) c.clock_hz = u;
    if (json_u32(j, "gamma_x10", 10, 40, u)) c.gamma_x10 = static_cast<uint8_t>(u);
    unsigned r, g, bl;
    if (hex_rgb(json_str(j, "wb"), r, g, bl)) {  // 0 would black a colour out: unity
        c.wb_r = r ? static_cast<uint8_t>(r) : 255;
        c.wb_g = g ? static_cast<uint8_t>(g) : 255;
        c.wb_b = bl ? static_cast<uint8_t>(bl) : 255;
    }
    if (json_u32(j, "fix_universe", 0, 32767, u))
        config::set_fix_address(c, static_cast<uint16_t>(u), config::fix_dmx_start(c));
    if (json_u32(j, "fix_dmx_start", 1, 512, u))
        config::set_fix_address(c, config::fix_universe(c), static_cast<uint16_t>(u));
    if ((s = json_str(j, "packing"))) {
        const int p = config::packing_from_id(s);
        // A body of before the two switches (an older backup, a script) names
        // neither: its layout then says it all — pixel mapping alone, or
        // "control": the fixtures alone, where the output is addressed.
        const bool before = !cJSON_HasObjectItem(j, "pixel_map") &&
                            !cJSON_HasObjectItem(j, "fixture_ctl");
        if (p == config::kPackControl) {
            config::set_fix_address(c, c.universe_start, c.dmx_start);
            config::set_dmx_modes(c, false, true);
        } else if (p >= 0) {
            config::set_pixel_layout(c, static_cast<uint8_t>(p));
            if (before) config::set_dmx_modes(c, true, false);
        } else {
            refuse(why, "packing: continuous|whole|fixture|colour");
        }
    }
    if (json_bool(j, "pixel_map", b)) config::set_dmx_modes(c, b, config::fixture_controlled(c));
    if (json_bool(j, "fixture_ctl", b)) config::set_dmx_modes(c, config::pixel_mapped(c), b);
    apply_gaps_json(j, c);
    apply_fixtures_json(j, c, why);
}

static void restore_global(cJSON* jg) {
    config::GlobalConfig g = config::get_global();
    apply_global_json(jg, g, nullptr);
    config::set_global(g);
}

static void restore_channel(size_t i, cJSON* jc) {
    auto c = config::get_channel(i);
    apply_channel_json(jc, c, nullptr);
    config::set_channel(i, c);
    dmx::mark_channel_dirty(i);
}

static bool parse_hex_color(const cJSON* it, uint8_t rgb[3]) {
    if (!cJSON_IsString(it)) return false;
    const char* cs = it->valuestring[0] == '#' ? it->valuestring + 1 : it->valuestring;
    unsigned r, g, b;
    if (sscanf(cs, "%02x%02x%02x", &r, &g, &b) != 3) return false;
    rgb[0] = static_cast<uint8_t>(r);
    rgb[1] = static_cast<uint8_t>(g);
    rgb[2] = static_cast<uint8_t>(b);
    return true;
}

static void apply_name_json(const cJSON* js, char* name, size_t cap) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(js, "name");
    if (!cJSON_IsString(it)) return;
    memset(name, 0, cap);
    strncpy(name, it->valuestring, cap - 1);
}

static void apply_u8_json(const cJSON* js, const char* key, double hi, uint8_t* out) {
    const cJSON* n = cJSON_GetObjectItemCaseSensitive(js, key);
    if (cJSON_IsNumber(n) && n->valuedouble >= 0 && n->valuedouble <= hi)
        *out = static_cast<uint8_t>(n->valuedouble);
}

// 1..kSceneColorsMax "#rrggbb" strings, all valid or nothing. Returns the count.
static int parse_colors_json(const cJSON* js, uint8_t out[config::kSceneColorsMax][3]) {
    const cJSON* cols = cJSON_GetObjectItemCaseSensitive(js, "colors");
    const int n       = cJSON_IsArray(cols) ? cJSON_GetArraySize(cols) : 0;
    if (n < 1 || static_cast<size_t>(n) > config::kSceneColorsMax) return 0;
    for (int k = 0; k < n; ++k)
        if (!parse_hex_color(cJSON_GetArrayItem(cols, k), out[k])) return 0;
    return n;
}

// Partial update: absent or out-of-range fields keep their current value.
void apply_effect_json(const cJSON* je, config::Effect& e) {
    apply_name_json(je, e.name, sizeof(e.name));
    apply_u8_json(je, "generator", config::kSceneFxCount - 1, &e.generator);
    apply_u8_json(je, "speed", 255, &e.speed);
    apply_u8_json(je, "param", 255, &e.param);
    uint8_t parsed[config::kSceneColorsMax][3];
    const int n = parse_colors_json(je, parsed);
    if (n) {
        std::memset(e.colors, 0, sizeof(e.colors));
        std::memcpy(e.colors, parsed, static_cast<size_t>(n) * 3);
        e.num_colors = static_cast<uint8_t>(n);
    }
    auto flag = [&](const cJSON* obj, const char* key, uint8_t bit) {
        const cJSON* b = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsBool(b))
            e.flags = static_cast<uint8_t>(cJSON_IsTrue(b) ? e.flags | bit : e.flags & ~bit);
    };
    flag(je, "invert", config::kEffectDimmerInvert);
    const cJSON* jm = cJSON_GetObjectItemCaseSensitive(je, "matricks");
    if (cJSON_IsObject(jm)) {
        apply_u8_json(jm, "block", 255, &e.block);
        apply_u8_json(jm, "groups", 255, &e.groups);
        apply_u8_json(jm, "wings", 255, &e.wings);
    }
    const cJSON* jp = cJSON_GetObjectItemCaseSensitive(je, "phaser");
    if (!cJSON_IsObject(jp)) return;
    const cJSON* jw = cJSON_GetObjectItemCaseSensitive(jp, "wave");
    const int wave  = cJSON_IsString(jw) ? config::phaser_wave_from_id(jw->valuestring) : -1;
    if (wave >= 0) e.ph_wave = static_cast<uint8_t>(wave);
    apply_u8_json(jp, "rate", 255, &e.ph_rate);
    apply_u8_json(jp, "spread", 255, &e.ph_spread);
    apply_u8_json(jp, "width", 255, &e.ph_width);
    apply_u8_json(jp, "low", 255, &e.ph_low);
    apply_u8_json(jp, "attack", 255, &e.ph_attack);
    apply_u8_json(jp, "decay", 255, &e.ph_decay);
    flag(jp, "reverse", config::kEffectPhaserReverse);
}

// Partial update. "parts", when present, replaces the scene's parts — and is
// taken whole or not at all: false, with the reason in *why, on a bad part.
bool apply_scene_json(const cJSON* js, config::Scene& sc, const char** why) {
    apply_name_json(js, sc.name, sizeof(sc.name));
    const cJSON* jgr = cJSON_GetObjectItemCaseSensitive(js, "group");  // -1 = its outputs
    if (cJSON_IsNumber(jgr) && jgr->valuedouble >= -1 &&
        jgr->valuedouble < static_cast<double>(config::kMaxGroups))
        sc.group = static_cast<uint8_t>(static_cast<int>(jgr->valuedouble) + 1);
    const cJSON* jparts = cJSON_GetObjectItemCaseSensitive(js, "parts");
    if (!jparts) return true;
    auto fail = [&](const char* msg) {
        *why = msg;
        return false;
    };
    if (!cJSON_IsArray(jparts) ||
        static_cast<size_t>(cJSON_GetArraySize(jparts)) > config::kMaxSceneParts)
        return fail("parts: an array of up to 8 parts");
    config::ScenePart parts[config::kMaxSceneParts] = {};
    size_t n                                        = 0;
    for (const cJSON* jp = jparts->child; jp; jp = jp->next) {
        const cJSON* jm = cJSON_GetObjectItemCaseSensitive(jp, "mask");
        const cJSON* jf = cJSON_GetObjectItemCaseSensitive(jp, "effect");
        const cJSON* jo = cJSON_GetObjectItemCaseSensitive(jp, "fixture_mode");
        if (!cJSON_IsNumber(jm) || !(jm->valuedouble >= 0 && jm->valuedouble <= 255))
            return fail("part mask: 0..255");
        if (!cJSON_IsNumber(jf) ||
            !(jf->valuedouble >= 0 && jf->valuedouble <= config::kMaxEffects - 1))
            return fail("part effect: an index in the effect bank");
        config::ScenePart& p = parts[n++];
        p.mask               = static_cast<uint8_t>(jm->valuedouble);
        p.effect             = static_cast<uint8_t>(jf->valuedouble);
        if (jo) {
            int mode = -1;
            for (uint8_t m = 0; cJSON_IsString(jo) && m < config::kFixtureModeCount; ++m)
                if (std::strcmp(jo->valuestring, config::fixture_mode_id(m)) == 0) mode = m;
            if (mode < 0) return fail("part fixture_mode: each, strip, chain or mirror");
            p.fixture_mode = static_cast<uint8_t>(mode);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(jp, "reverse")))
            p.fixture_mode |= config::kSceneReverseBit;
    }
    std::memcpy(sc.parts, parts, sizeof(parts));
    sc.num_parts = static_cast<uint8_t>(n);
    return true;
}

// A scene as backups up to version 1 wrote it: the look and its outputs in one
// object ("effect" was the generator, "speed" in the old, half-as-fast scale).
static void apply_scene_v3_json(const cJSON* js, config::SceneV3& sc) {
    apply_name_json(js, sc.name, sizeof(sc.name));
    apply_u8_json(js, "effect", config::kSceneFxCount - 1, &sc.effect);
    apply_u8_json(js, "speed", 255, &sc.speed);
    apply_u8_json(js, "param", 255, &sc.param);
    apply_u8_json(js, "mask", 255, &sc.channel_mask);
    // fixture_mode, reverse and group share one byte (pack_scene_mode).
    uint8_t mode    = config::scene_mode_of(sc.fixture_mode);
    bool reverse    = config::scene_reverse_of(sc.fixture_mode);
    int group       = config::scene_group_of(sc.fixture_mode);
    const cJSON* fm = cJSON_GetObjectItemCaseSensitive(js, "fixture_mode");
    for (uint8_t m = 0; cJSON_IsString(fm) && m < config::kFixtureModeCount; ++m)
        if (std::strcmp(fm->valuestring, config::fixture_mode_id(m)) == 0) mode = m;
    const cJSON* jrv = cJSON_GetObjectItemCaseSensitive(js, "reverse");
    if (cJSON_IsBool(jrv)) reverse = cJSON_IsTrue(jrv);
    const cJSON* jgr = cJSON_GetObjectItemCaseSensitive(js, "group");  // -1 = its outputs
    if (cJSON_IsNumber(jgr) && jgr->valuedouble >= -1 &&
        jgr->valuedouble < static_cast<double>(config::kMaxGroups))
        group = static_cast<int>(jgr->valuedouble);
    sc.fixture_mode = config::pack_scene_mode(mode, reverse, group);
    uint8_t rgb[3];
    uint8_t parsed[config::kSceneColorsMax][3];
    const int n = parse_colors_json(js, parsed);
    if (n) {
        for (int k = 0; k < n; ++k)
            config::set_scene_color(sc, static_cast<size_t>(k), parsed[k][0], parsed[k][1],
                                    parsed[k][2]);
        sc.num_colors = static_cast<uint8_t>(n);
    } else if (parse_hex_color(cJSON_GetObjectItemCaseSensitive(js, "color"), rgb)) {
        config::set_scene_color(sc, 0, rgb[0], rgb[1], rgb[2]);
    }
}

// The backup's effect bank and scene list replace the current ones wholesale
// (lengths included). A backup without "effects" comes from a firmware whose
// scenes carried their own look: each becomes an effect and a scene.
static void restore_scenes(const cJSON* jfx, const cJSON* jsc) {
    struct Work {  // 4 kB: neither on the httpd stack nor in internal RAM for good
        config::EffectBank effects;
        config::SceneBank scenes;
        config::SceneBankV3 old;
    };
    auto* w = static_cast<Work*>(heap_caps_malloc(sizeof(Work), MALLOC_CAP_SPIRAM));
    if (!w) w = static_cast<Work*>(malloc(sizeof(Work)));
    if (!w) return;
    std::memset(w, 0, sizeof(Work));
    if (cJSON_IsArray(jfx)) {
        for (const cJSON* je = jfx->child; je && w->effects.count < config::kMaxEffects;
             je              = je->next) {
            config::Effect& e = w->effects.effects[w->effects.count++];
            e.num_colors      = 1;
            apply_effect_json(je, e);
        }
        for (const cJSON* js = cJSON_IsArray(jsc) ? jsc->child : nullptr;
             js && w->scenes.count < config::kMaxScenes; js = js->next) {
            const char* why = nullptr;
            apply_scene_json(js, w->scenes.scenes[w->scenes.count++], &why);
        }
    } else {
        for (const cJSON* js = jsc->child; js && w->old.count < config::kMaxScenes; js = js->next) {
            config::SceneV3& sc = w->old.scenes[w->old.count++];
            sc.channel_mask     = 0xFF;
            sc.num_colors       = 1;
            apply_scene_v3_json(js, sc);
        }
        config::migrate_scenes_v3(w->old, w->effects, w->scenes);
    }
    dmx::scene_stop();  // the playing index may point elsewhere now
    config::replace_effects(w->effects.effects, w->effects.count);
    config::replace_scenes(w->scenes.scenes, w->scenes.count);
    heap_caps_free(w);
}

// "network": where the box now answers — its static address, or null while
// it waits for a DHCP lease (its mDNS name, in /api/status, still finds it).
void add_network_applied(cJSON* resp, const config::GlobalConfig& g) {
    cJSON* n = cJSON_AddObjectToObject(resp, "network");
    cJSON_AddBoolToObject(n, "applied", true);
    cJSON_AddBoolToObject(n, "dhcp", g.use_dhcp || g.static_ip == 0);
    if (!g.use_dhcp && g.static_ip != 0) {
        char ip[16];
        fmt_ip(ip, sizeof(ip), g.static_ip);
        cJSON_AddStringToObject(n, "ip", ip);
    } else {
        cJSON_AddNullToObject(n, "ip");
    }
}

esp_err_t handle_restore(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    // Full backup ≈ 3 kB + ~130 B per effect (31 max) + up to ~450 B per scene
    // (30 max, 8 parts each) + up to ~400 B of fixtures per channel + the
    // groups. In PSRAM, once: that much internal RAM is dear.
    constexpr size_t kRestoreMax = 32768;
    static char* buf             = nullptr;
    if (!buf) buf = static_cast<char*>(heap_caps_malloc(kRestoreMax, MALLOC_CAP_SPIRAM));
    if (!buf) buf = static_cast<char*>(malloc(kRestoreMax));
    if (!buf) return send_err(req, 500, "out of memory");
    if (!read_body(req, buf, kRestoreMax - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    cJSON* jfx = cJSON_GetObjectItemCaseSensitive(j, "effects");
    cJSON* jsc = cJSON_GetObjectItemCaseSensitive(j, "scenes");
    if (cJSON_IsArray(jfx) || cJSON_IsArray(jsc)) restore_scenes(jfx, jsc);
    const config::GlobalConfig before = config::get_global();
    cJSON* jg                         = cJSON_GetObjectItemCaseSensitive(j, "global");
    if (cJSON_IsObject(jg)) restore_global(jg);
    // Control mode: applied only when the whole object is valid.
    cJSON* jctl = cJSON_GetObjectItemCaseSensitive(j, "control");
    if (cJSON_IsObject(jctl)) {
        config::ControlConfig c = config::get_control();
        const char* why         = nullptr;
        if (apply_control_json(jctl, c, &why)) config::set_control(c);
    }
    // Playlist: likewise all or nothing.
    cJSON* jpl = cJSON_GetObjectItemCaseSensitive(j, "playlist");
    if (cJSON_IsObject(jpl)) {
        config::FseqPlaylist p = config::get_playlist();
        const char* why        = nullptr;
        if (apply_playlist_json(jpl, p, &why)) config::set_playlist(p);
    }
    // Groups: likewise all or nothing.
    cJSON* jgr = cJSON_GetObjectItemCaseSensitive(j, "groups");
    if (cJSON_IsArray(jgr)) {
        static config::GroupsConfig gr;  // 2.4 kB: off the httpd stack
        const char* why = nullptr;
        if (apply_groups_json(jgr, gr, &why)) config::set_groups(gr);
    }
    // Profiles: likewise all or nothing, and before the channels whose
    // fixtures point at them.
    cJSON* jpr = cJSON_GetObjectItemCaseSensitive(j, "profiles");
    if (cJSON_IsArray(jpr)) {
        static config::ProfileBank pb;
        const char* why = nullptr;
        if (apply_profiles_json(jpr, pb, &why)) config::set_profiles(pb);
    }
    cJSON* jchs = cJSON_GetObjectItemCaseSensitive(j, "channels");
    if (cJSON_IsArray(jchs)) {
        const int n = cJSON_GetArraySize(jchs);
        for (int i = 0; i < n && i < static_cast<int>(config::kNumChannels); ++i)
            restore_channel(static_cast<size_t>(i), cJSON_GetArrayItem(jchs, i));
    }
    cJSON_Delete(j);
    dmx::mark_global_dirty();

    // Same live side effects as POST /api/global: the receivers follow their
    // flags, and the box its addressing, now — not at the next reboot.
    const config::GlobalConfig& g = config::get_global();
    const bool network_changed = g.use_dhcp != before.use_dhcp || g.static_ip != before.static_ip ||
                                 g.static_mask != before.static_mask ||
                                 g.static_gateway != before.static_gateway;
    if (network_changed) net::apply(g);
    if (g.sacn_enabled != before.sacn_enabled) {
        if (g.sacn_enabled)
            sacn::start();
        else
            sacn::stop();
    }
    if (g.fpp_remote != before.fpp_remote) {
        if (g.fpp_remote)
            fpp::start();
        else
            fpp::stop();
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (network_changed) add_network_applied(resp, g);
    return send_json(req, resp);
}

// A handler cannot stop the server it runs in (httpd_stop waits for it):
// answer first, then stop from a short-lived task.
static void web_stop_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(300));
    web::stop();
    vTaskDelete(nullptr);
}

// ── POST /api/global ─────────────────────────────────────────────────────────

esp_err_t handle_post_global(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[512];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");

    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    config::GlobalConfig g = config::get_global();
    const char* why        = nullptr;
    const GlobalApplied fx = apply_global_json(j, g, &why);
    if (why) {
        cJSON_Delete(j);
        return send_err(req, 400, why);
    }
    const bool network_changed = fx.network, web_off = fx.web_off;
    const bool sacn_changed = fx.sacn, fpp_changed = fx.fpp;
    const char* s;

    // Admin password: separate setter (hashes + persists on its own); empty
    // string clears it (auth off). Copied out before cJSON_Delete frees the
    // backing buffer. Never echoed back. Too long is refused, not truncated:
    // a truncated hash would never match what the browser sends back.
    char pwd[config::kMaxWebPasswordLen + 1];
    bool password_changed = false;
    if ((s = json_str(j, "web_password"))) {
        if (strlen(s) > config::kMaxWebPasswordLen) {
            cJSON_Delete(j);
            return send_err(req, 400, "web_password: at most 63 characters");
        }
        strcpy(pwd, s);
        password_changed = true;
    }

    cJSON_Delete(j);
    config::set_global(g);
    if (password_changed) config::set_web_password(pwd);
    dmx::mark_global_dirty();

    if (sacn_changed) {
        if (g.sacn_enabled)
            sacn::start();
        else
            sacn::stop();
    }
    if (fpp_changed) {
        if (g.fpp_remote)
            fpp::start();
        else
            fpp::stop();
    }
    // The box re-addresses itself now: the page that asked must reconnect
    // (the answer says where to; nowhere known with DHCP).
    if (network_changed) net::apply(g);

    // Turning the web UI off from the web UI: this server stops right after
    // the answer (it used to keep serving until a reboot).
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (network_changed) add_network_applied(resp, g);
    if (web_off) cJSON_AddBoolToObject(resp, "web_stopping", true);
    const esp_err_t r = send_json(req, resp);
    if (web_off && xTaskCreate(web_stop_task, "web_stop", 3072, nullptr, 5, nullptr) != pdPASS)
        ESP_LOGE(TAG, "web_stop task create failed: the server runs until reboot");
    return r;
}

// ── POST /api/channel/{n} ────────────────────────────────────────────────────

esp_err_t handle_post_channel(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    // URI: /api/channel/N or /api/channel/N/identify
    const char* tail = req->uri + strlen("/api/channel/");
    const int idx    = atoi(tail);
    if (idx < 0 || static_cast<size_t>(idx) >= config::kNumChannels)
        return send_err(req, 400, "channel 0..7");
    if (strstr(tail, "/identify") != nullptr) {
        dmx::identify_start(static_cast<size_t>(idx));
        return send_ok(req);
    }

    static char buf[1536];  // 32 fixtures + 8 gaps + the rest; off the httpd stack
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");

    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    config::ChannelConfig c = config::get_channel(static_cast<size_t>(idx));
    const char* why         = nullptr;
    apply_channel_json(j, c, &why);
    if (why) {
        cJSON_Delete(j);
        return send_err(req, 400, why);
    }

    cJSON_Delete(j);
    config::set_channel(static_cast<size_t>(idx), c);
    dmx::mark_channel_dirty(static_cast<size_t>(idx));
    return send_ok(req);
}

// ── POST /api/identify ───────────────────────────────────────────────────────
// Blink outputs one after the other (dmx::identify_outputs), each 3 times.
// Body {"outputs": mask} or empty / {} = every configured output. Replies
// the mask actually run.
esp_err_t handle_identify(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[64] = "";
    uint8_t mask = dmx::identify_configured_outputs();
    if (req->content_len) {
        if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large");
        cJSON* j = cJSON_Parse(buf);
        if (!j) return send_err(req, 400, "invalid JSON");
        uint32_t u = 0;
        if (json_u32(j, "outputs", 0, 255, u)) mask = static_cast<uint8_t>(u);
        cJSON_Delete(j);
    }
    dmx::identify_outputs(mask);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "outputs", mask);
    return send_json(req, root);
}

// ── POST /api/autopatch ─────────────────────────────────────────────────────
// Re-address every channel one after the other from a base universe. Body:
// {"base": 0..32767, "compact": bool (share universes), "packing": "keep" |
// "continuous" | "whole" | "fixture" (set on every channel; default keep)}.
// Replies the next free universe and the universes used against the pool.
esp_err_t handle_autopatch(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[128];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    cJSON* base_item = cJSON_GetObjectItemCaseSensitive(j, "base");
    const bool ok    = base_item && cJSON_IsNumber(base_item) && base_item->valuedouble >= 0 &&
                    base_item->valuedouble <= 32767;
    dmx::AutoPatch o;
    o.base = ok ? static_cast<uint16_t>(base_item->valuedouble) : 0;
    bool b = false;
    if (json_bool(j, "compact", b)) o.compact = b;
    const char* p     = json_str(j, "packing");
    const int packing = p && std::strcmp(p, "keep") != 0 ? config::packing_from_id(p) : -1;
    const bool bad_p  = p && std::strcmp(p, "keep") != 0 && packing < 0;
    o.packing         = static_cast<int8_t>(packing);
    // "fix_base": where the fixtures' block starts (absent or -1: after the pixels).
    const cJSON* fb   = cJSON_GetObjectItemCaseSensitive(j, "fix_base");
    const bool bad_fb = fb && !cJSON_IsNull(fb) &&
                        !(cJSON_IsNumber(fb) && fb->valuedouble >= -1 && fb->valuedouble <= 32767);
    if (cJSON_IsNumber(fb) && !bad_fb) o.fix_base = static_cast<int32_t>(fb->valuedouble);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, "base 0..32767");
    if (bad_fb) return send_err(req, 400, "fix_base 0..32767 (-1 = after the pixels)");
    // "control" is an output's mode, not a pixel layout to give every output.
    if (bad_p || packing == config::kPackControl)
        return send_err(req, 400, "packing: keep|continuous|whole|fixture|colour");

    uint16_t next    = 0;
    size_t universes = 0;
    dmx::auto_patch(o, &next, &universes);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "next_free", next);
    cJSON_AddNumberToObject(root, "universes", static_cast<double>(universes));
    cJSON_AddNumberToObject(root, "pool", static_cast<double>(dmx::kNumUniverses));
    return send_json(req, root);
}

// ── Fixture groups ──────────────────────────────────────────────────────────
// [{"name": "Top", "members": [[output, fixture], ...]}, ...] — 0-based, the
// members in the group's order (the strip a scene runs along).

cJSON* build_groups_json() {
    const auto& g = config::get_groups();
    cJSON* arr    = cJSON_CreateArray();
    for (size_t i = 0; i < g.count; ++i) {
        const auto& fg = g.groups[i];
        cJSON* jg      = cJSON_CreateObject();
        cJSON_AddStringToObject(jg, "name", fg.name);
        cJSON* jm = cJSON_AddArrayToObject(jg, "members");
        for (size_t m = 0; m < fg.count; ++m) {
            cJSON* pair = cJSON_CreateArray();
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(fg.members[m].output));
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(fg.members[m].fixture));
            cJSON_AddItemToArray(jm, pair);
        }
        cJSON_AddItemToArray(arr, jg);
    }
    return arr;
}

// All or nothing: a malformed group refuses the whole list (`g` untouched
// only on success matters to the caller, which then stores it).
bool apply_groups_json(const cJSON* j, config::GroupsConfig& g, const char** why) {
    if (!cJSON_IsArray(j) || cJSON_GetArraySize(j) > static_cast<int>(config::kMaxGroups)) {
        if (why) *why = "groups: a list of at most 16";
        return false;
    }
    g = config::GroupsConfig{};
    for (const cJSON* jg = j->child; jg; jg = jg->next) {
        config::FixtureGroup& fg = g.groups[g.count];
        const cJSON* jn          = cJSON_GetObjectItemCaseSensitive(jg, "name");
        const cJSON* jm          = cJSON_GetObjectItemCaseSensitive(jg, "members");
        if (!cJSON_IsString(jn) || !cJSON_IsArray(jm) ||
            cJSON_GetArraySize(jm) > static_cast<int>(config::kMaxGroupMembers)) {
            if (why) *why = "groups: {name, members: at most 64 [output, fixture]}";
            return false;
        }
        std::strncpy(fg.name, jn->valuestring, config::kGroupNameMax - 1);
        for (const cJSON* p = jm->child; p; p = p->next) {
            const cJSON* jo = cJSON_GetArrayItem(p, 0);
            const cJSON* jf = cJSON_GetArrayItem(p, 1);
            if (!cJSON_IsNumber(jo) || !cJSON_IsNumber(jf) || jo->valuedouble < 0 ||
                jo->valuedouble >= config::kNumChannels || jf->valuedouble < 0 ||
                jf->valuedouble >= config::kMaxFixtures) {
                if (why) *why = "groups: member [output 0..7, fixture 0..31]";
                return false;
            }
            fg.members[fg.count++] = config::FixtureRef{ static_cast<uint8_t>(jo->valuedouble),
                                                         static_cast<uint8_t>(jf->valuedouble) };
        }
        ++g.count;
    }
    return true;
}

// ── POST /api/groups ────────────────────────────────────────────────────────
// {"groups": [...]} replaces the whole list. Up to ~9 kB: in PSRAM, once.
char* big_body_buffer() {
    static char* buf = nullptr;
    if (!buf) buf = static_cast<char*>(heap_caps_malloc(kBigBodyMax, MALLOC_CAP_SPIRAM));
    if (!buf) buf = static_cast<char*>(malloc(kBigBodyMax));
    return buf;
}

esp_err_t handle_post_groups(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char* buf = big_body_buffer();
    if (!buf) return send_err(req, 500, "out of memory");
    if (!read_body(req, buf, kBigBodyMax - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    static config::GroupsConfig g;  // 2.4 kB: off the httpd stack
    const char* why = nullptr;
    const bool ok   = apply_groups_json(cJSON_GetObjectItemCaseSensitive(j, "groups"), g, &why);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, why);
    config::set_groups(g);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "groups", build_groups_json());
    return send_json(req, root);
}

}  // namespace pixfrog::web::impl
