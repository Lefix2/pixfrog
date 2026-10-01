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
    char fscol[8];
    snprintf(fscol, sizeof(fscol), "#%02x%02x%02x", g.failsafe_r, g.failsafe_g, g.failsafe_b);
    cJSON_AddStringToObject(jg, "failsafe_color", fscol);
    return jg;
}

static cJSON* build_scenes_json() {
    cJSON* jscenes = cJSON_CreateArray();
    for (size_t i = 0; i < config::num_scenes(); ++i) {
        const auto& sc = config::get_scene(i);
        cJSON* js      = cJSON_CreateObject();
        cJSON_AddStringToObject(js, "name", sc.name);
        cJSON_AddNumberToObject(js, "effect", sc.effect);
        char col[8];
        snprintf(col, sizeof(col), "#%02x%02x%02x", sc.r, sc.g, sc.b);
        cJSON_AddStringToObject(js, "color", col);
        cJSON* jcols = cJSON_AddArrayToObject(js, "colors");
        for (size_t k = 0; k < config::scene_num_colors(sc); ++k) {
            uint8_t rgb[3];
            config::scene_color(sc, k, rgb);
            snprintf(col, sizeof(col), "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
            cJSON_AddItemToArray(jcols, cJSON_CreateString(col));
        }
        cJSON_AddNumberToObject(js, "speed", sc.speed);
        cJSON_AddNumberToObject(js, "param", sc.param);
        cJSON_AddNumberToObject(js, "mask", sc.channel_mask);
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
    cJSON_AddItemToObject(root, "scenes", build_scenes_json());
    cJSON_AddItemToObject(root, "channels", build_channels_json());
    cJSON_AddItemToObject(root, "control", build_control_json());
    cJSON_AddItemToObject(root, "playlist", build_playlist_json());
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
    cJSON_AddNumberToObject(root, "backup_version", 1);
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddItemToObject(root, "global", build_global_json());
    cJSON_AddItemToObject(root, "channels", build_channels_json());
    cJSON_AddItemToObject(root, "scenes", build_scenes_json());
    cJSON_AddItemToObject(root, "control", build_control_json());
    cJSON_AddItemToObject(root, "playlist", build_playlist_json());
    static char disposition[96];  // httpd keeps the pointer until the send
    backup_filename(req, disposition, sizeof(disposition));
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    return send_json(req, root);
}

static void restore_global(cJSON* jg) {
    config::GlobalConfig g = config::get_global();
    cJSON* it;
    auto num = [&](const char* k, double lo, double hi, double* out) {
        it = cJSON_GetObjectItemCaseSensitive(jg, k);
        if (cJSON_IsNumber(it) && it->valuedouble >= lo && it->valuedouble <= hi) {
            *out = it->valuedouble;
            return true;
        }
        return false;
    };
    auto getb = [&](const char* k, bool* out) {
        it = cJSON_GetObjectItemCaseSensitive(jg, k);
        if (cJSON_IsBool(it)) {
            *out = cJSON_IsTrue(it);
            return true;
        }
        return false;
    };
    auto getip = [&](const char* k, uint32_t* out) {
        it = cJSON_GetObjectItemCaseSensitive(jg, k);
        uint32_t x;
        if (cJSON_IsString(it) && parse_ip(it->valuestring, x)) *out = x;
    };
    double v;
    bool bv;
    if (getb("dhcp", &bv)) g.use_dhcp = bv;
    it = cJSON_GetObjectItemCaseSensitive(jg, "ip_fallback");
    if (cJSON_IsString(it) && config::ip_fallback_from_id(it->valuestring) >= 0)
        g.ip_fallback = static_cast<uint8_t>(config::ip_fallback_from_id(it->valuestring));
    getip("ip", &g.static_ip);
    getip("mask", &g.static_mask);
    getip("gw", &g.static_gateway);
    if (num("net", 0, 127, &v)) g.artnet_net = static_cast<uint8_t>(v);
    if (num("subnet", 0, 15, &v)) g.artnet_subnet = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jg, "short_name");
    if (cJSON_IsString(it)) {
        memset(g.short_name, 0, sizeof(g.short_name));
        strncpy(g.short_name, it->valuestring, sizeof(g.short_name) - 1);
    }
    it = cJSON_GetObjectItemCaseSensitive(jg, "long_name");
    if (cJSON_IsString(it)) {
        memset(g.long_name, 0, sizeof(g.long_name));
        strncpy(g.long_name, it->valuestring, sizeof(g.long_name) - 1);
    }
    if (getb("reply_unicast", &bv)) g.artnet_poll_reply_unicast = bv;
    if (num("refresh_hz", config::kMinRefreshHz, config::kMaxRefreshHz, &v))
        g.refresh_rate_hz = static_cast<uint8_t>(v);
    if (num("home_timeout_s", 0, 65535, &v)) g.home_timeout_s = static_cast<uint16_t>(v);
    if (num("tft_brightness", config::kTftBrightnessMin, 100, &v))
        g.tft_brightness = static_cast<uint8_t>(v);
    if (num("tft_idle_dim", 0, 100, &v)) g.tft_idle_dim = static_cast<uint8_t>(v);
    if (num("tft_dim_delay_s", 0, config::kTftDimDelayMaxS, &v))
        g.tft_dim_delay_s = static_cast<uint16_t>(v);
    if (getb("web_enabled", &bv)) g.web_enabled = bv;
    if (getb("sacn_enabled", &bv)) g.sacn_enabled = bv;
    if (num("failsafe_mode", 0, 3, &v)) g.failsafe_mode = static_cast<uint8_t>(v);
    if (num("failsafe_timeout_s", 0, 3600, &v)) g.failsafe_timeout_s = static_cast<uint16_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jg, "failsafe_color");
    if (cJSON_IsString(it)) {
        unsigned cr, cg, cb;
        if (sscanf(it->valuestring[0] == '#' ? it->valuestring + 1 : it->valuestring,
                   "%02x%02x%02x", &cr, &cg, &cb) == 3) {
            g.failsafe_r = static_cast<uint8_t>(cr);
            g.failsafe_g = static_cast<uint8_t>(cg);
            g.failsafe_b = static_cast<uint8_t>(cb);
        }
    }
    // Bounded by the list capacity, not its current length: a reference past
    // the end plays nothing (scene_start / get_scene bound-check at use).
    if (num("failsafe_scene", 0, config::kMaxScenes - 1, &v))
        g.failsafe_scene = static_cast<uint8_t>(v);
    if (num("boot_scene", 0, config::kMaxScenes, &v)) g.boot_scene = static_cast<uint8_t>(v);
    if (num("merge_mode", 0, 1, &v)) g.merge_mode = static_cast<uint8_t>(v);
    if (getb("fpp_remote", &bv)) g.fpp_remote = bv;
    if (num("lang", 0, 1, &v)) g.language = static_cast<uint8_t>(v);
    if (getb("hub_preferred", &bv)) g.hub_preferred = bv;
    if (num("scene_fade_ms", 0, config::kMaxSceneFadeMs, &v))
        g.scene_fade_ms = static_cast<uint16_t>(v);
    if (num("fseq_universe", 1, dmx::kMaxUniverseNumber, &v))
        g.fseq_universe = static_cast<uint16_t>(v);
    config::set_global(g);
}

static void restore_channel(size_t i, cJSON* jc) {
    auto c    = config::get_channel(i);
    cJSON* it = cJSON_GetObjectItemCaseSensitive(jc, "protocol");
    if (cJSON_IsString(it)) {
        const int pv = lookup(kProtoNames, static_cast<size_t>(led::Protocol::COUNT),
                              it->valuestring);
        if (pv >= 0) c.protocol = static_cast<led::Protocol>(pv);
        // A backup from before DMX512 output moved to the DMX node firmware:
        // that channel described a DMX universe, not a strip — disable it.
        if (std::strcmp(it->valuestring, "DMX512") == 0) c.protocol = led::Protocol::Off;
    }
    it = cJSON_GetObjectItemCaseSensitive(jc, "color_order");
    if (cJSON_IsString(it)) {
        const int ov = lookup(kOrderNames, static_cast<size_t>(led::ColorOrder::COUNT),
                              it->valuestring);
        if (ov >= 0) c.color_order = static_cast<led::ColorOrder>(ov);
    }
    auto num = [&](const char* k, double lo, double hi, double* out) {
        it = cJSON_GetObjectItemCaseSensitive(jc, k);
        if (cJSON_IsNumber(it) && it->valuedouble >= lo && it->valuedouble <= hi) {
            *out = it->valuedouble;
            return true;
        }
        return false;
    };
    double v;
    if (num("universe_start", 0, 32767, &v)) c.universe_start = static_cast<uint16_t>(v);
    if (num("dmx_start", 1, 512, &v)) c.dmx_start = static_cast<uint16_t>(v);
    if (num("pixel_count", 1, dmx::kMaxPixelsPerChan, &v)) c.pixel_count = static_cast<uint16_t>(v);
    if (num("brightness", 0, 255, &v)) c.brightness = static_cast<uint8_t>(v);
    if (num("grouping", 1, 8, &v)) c.grouping = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jc, "invert");
    if (cJSON_IsBool(it)) c.invert_direction = cJSON_IsTrue(it);
    if (num("clock_hz", led::kMinClockHz, led::kMaxClockHz, &v))
        c.clock_hz = static_cast<uint32_t>(v);
    if (num("gamma_x10", 10, 40, &v)) c.gamma_x10 = static_cast<uint8_t>(v);
    it = cJSON_GetObjectItemCaseSensitive(jc, "wb");
    if (cJSON_IsString(it)) {
        unsigned cr, cg, cb;
        if (sscanf(it->valuestring[0] == '#' ? it->valuestring + 1 : it->valuestring,
                   "%02x%02x%02x", &cr, &cg, &cb) == 3) {
            c.wb_r = cr ? static_cast<uint8_t>(cr) : 255;
            c.wb_g = cg ? static_cast<uint8_t>(cg) : 255;
            c.wb_b = cb ? static_cast<uint8_t>(cb) : 255;
        }
    }
    apply_gaps_json(jc, c);
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

// Partial update: absent or out-of-range fields keep their current value.
// "colors" (1..kSceneColorsMax) wins over the single legacy "color".
void apply_scene_json(const cJSON* js, config::Scene& sc) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(js, "name");
    if (cJSON_IsString(it)) {
        memset(sc.name, 0, sizeof(sc.name));
        strncpy(sc.name, it->valuestring, sizeof(sc.name) - 1);
    }
    auto num = [&](const char* k, double hi, uint8_t* out) {
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(js, k);
        if (cJSON_IsNumber(n) && n->valuedouble >= 0 && n->valuedouble <= hi)
            *out = static_cast<uint8_t>(n->valuedouble);
    };
    num("effect", config::kSceneFxCount - 1, &sc.effect);
    num("speed", 255, &sc.speed);
    num("param", 255, &sc.param);
    num("mask", 255, &sc.channel_mask);

    uint8_t rgb[3];
    const cJSON* cols = cJSON_GetObjectItemCaseSensitive(js, "colors");
    const int ncols   = cJSON_IsArray(cols) ? cJSON_GetArraySize(cols) : 0;
    if (ncols >= 1 && static_cast<size_t>(ncols) <= config::kSceneColorsMax) {
        uint8_t parsed[config::kSceneColorsMax][3];
        bool ok = true;
        for (int k = 0; k < ncols && ok; ++k)
            ok = parse_hex_color(cJSON_GetArrayItem(cols, k), parsed[k]);
        if (ok) {
            for (int k = 0; k < ncols; ++k)
                config::set_scene_color(sc, static_cast<size_t>(k), parsed[k][0], parsed[k][1],
                                        parsed[k][2]);
            sc.num_colors = static_cast<uint8_t>(ncols);
        }
    } else if (parse_hex_color(cJSON_GetObjectItemCaseSensitive(js, "color"), rgb)) {
        config::set_scene_color(sc, 0, rgb[0], rgb[1], rgb[2]);
    }
}

// The backup's list replaces the current one wholesale (length included).
static void restore_scenes(const cJSON* jsc) {
    static config::Scene list[config::kMaxScenes];  // httpd stack is small
    size_t n = 0;
    for (const cJSON* js = jsc->child; js && n < config::kMaxScenes; js = js->next) {
        list[n]              = config::Scene{};
        list[n].channel_mask = 0xFF;
        list[n].num_colors   = 1;
        apply_scene_json(js, list[n++]);
    }
    dmx::scene_stop();  // the playing index may point elsewhere now
    config::replace_scenes(list, n);
}

esp_err_t handle_restore(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    // Full backup ≈ 3 kB + ~170 B per scene (30 max); static keeps it off the httpd stack.
    static char buf[12288];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    cJSON* jsc = cJSON_GetObjectItemCaseSensitive(j, "scenes");
    if (cJSON_IsArray(jsc)) restore_scenes(jsc);
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
    cJSON* jchs = cJSON_GetObjectItemCaseSensitive(j, "channels");
    if (cJSON_IsArray(jchs)) {
        const int n = cJSON_GetArraySize(jchs);
        for (int i = 0; i < n && i < static_cast<int>(config::kNumChannels); ++i)
            restore_channel(static_cast<size_t>(i), cJSON_GetArrayItem(jchs, i));
    }
    cJSON_Delete(j);
    dmx::mark_global_dirty();

    // Same live side effects as POST /api/global: the receivers follow their
    // flags now, not at the next reboot.
    const config::GlobalConfig& g = config::get_global();
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

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":true,\"note\":\"network/web changes apply after reboot\"}");
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
    bool network_changed   = false;

    auto get_bool = [&](const char* key, bool& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item) return false;
        if (cJSON_IsBool(item)) {
            out = cJSON_IsTrue(item);
            return true;
        }
        if (cJSON_IsNumber(item)) {
            out = (item->valuedouble != 0);
            return true;
        }
        return false;
    };
    auto get_u32 = [&](const char* key, uint32_t lo, uint32_t hi, uint32_t& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsNumber(item)) return false;
        // Range-check the double first: casting an out-of-range value
        // (-5, 1e40) to uint32_t is undefined (RISC-V saturates -5 to 0).
        const double d = item->valuedouble;
        if (!(d >= lo && d <= hi)) return false;
        out = static_cast<uint32_t>(d);
        return true;
    };
    auto get_str = [&](const char* key) -> const char* {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsString(item)) return nullptr;
        return item->valuestring;
    };

    uint32_t u = 0;
    bool b     = false;
    const char* s;

    if (get_bool("dhcp", b)) {
        g.use_dhcp      = b;
        network_changed = true;
    }
    if ((s = get_str("ip"))) {
        if (!parse_ip(s, g.static_ip)) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad ip");
        }
        network_changed = true;
    }
    if ((s = get_str("mask"))) {
        if (!parse_ip(s, g.static_mask)) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad mask");
        }
        network_changed = true;
    }
    if ((s = get_str("gw"))) {
        if (!parse_ip(s, g.static_gateway)) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad gw");
        }
        network_changed = true;
    }
    // Read when the fallback is taken: no reboot needed.
    if ((s = get_str("ip_fallback"))) {
        const int fb = config::ip_fallback_from_id(s);
        if (fb < 0) {
            cJSON_Delete(j);
            return send_err(req, 400, "ip_fallback: linklocal|artnet");
        }
        g.ip_fallback = static_cast<uint8_t>(fb);
    }
    if (get_u32("net", 0, 127, u)) g.artnet_net = static_cast<uint8_t>(u);
    if (get_u32("subnet", 0, 15, u)) g.artnet_subnet = static_cast<uint8_t>(u);
    if ((s = get_str("short_name"))) {
        memset(g.short_name, 0, sizeof(g.short_name));
        strncpy(g.short_name, s, sizeof(g.short_name) - 1);
    }
    if ((s = get_str("long_name"))) {
        memset(g.long_name, 0, sizeof(g.long_name));
        strncpy(g.long_name, s, sizeof(g.long_name) - 1);
    }
    if (get_bool("reply_unicast", b)) g.artnet_poll_reply_unicast = b;
    if (get_u32("refresh_hz", config::kMinRefreshHz, config::kMaxRefreshHz, u))
        g.refresh_rate_hz = static_cast<uint8_t>(u);
    if (get_u32("home_timeout_s", 0, 65535, u)) g.home_timeout_s = static_cast<uint16_t>(u);
    if (get_u32("tft_brightness", config::kTftBrightnessMin, 100, u))
        g.tft_brightness = static_cast<uint8_t>(u);
    if (get_u32("tft_idle_dim", 0, 100, u)) g.tft_idle_dim = static_cast<uint8_t>(u);
    if (get_u32("tft_dim_delay_s", 0, config::kTftDimDelayMaxS, u))
        g.tft_dim_delay_s = static_cast<uint16_t>(u);
    bool web_off = false;  // this request turns the web UI off
    if (get_bool("web_enabled", b)) {
        g.web_enabled = b;
        web_off       = !b;
    }
    if (get_u32("failsafe_mode", 0, 3, u)) g.failsafe_mode = static_cast<uint8_t>(u);
    if (get_u32("failsafe_scene", 0, config::kMaxScenes - 1, u))
        g.failsafe_scene = static_cast<uint8_t>(u);
    if (get_u32("boot_scene", 0, config::kMaxScenes, u)) g.boot_scene = static_cast<uint8_t>(u);
    if (get_u32("failsafe_timeout_s", 0, 3600, u)) g.failsafe_timeout_s = static_cast<uint16_t>(u);
    if (get_u32("merge_mode", 0, 1, u)) g.merge_mode = static_cast<uint8_t>(u);
    if (get_u32("lang", 0, 1, u)) g.language = static_cast<uint8_t>(u);
    if (get_bool("hub_preferred", b)) g.hub_preferred = b;
    if (get_u32("scene_fade_ms", 0, config::kMaxSceneFadeMs, u))
        g.scene_fade_ms = static_cast<uint16_t>(u);
    if (get_u32("fseq_universe", 1, dmx::kMaxUniverseNumber, u))
        g.fseq_universe = static_cast<uint16_t>(u);
    if ((s = get_str("failsafe_color"))) {
        unsigned fr, fg, fb;
        if (sscanf(s[0] == '#' ? s + 1 : s, "%02x%02x%02x", &fr, &fg, &fb) == 3) {
            g.failsafe_r = static_cast<uint8_t>(fr);
            g.failsafe_g = static_cast<uint8_t>(fg);
            g.failsafe_b = static_cast<uint8_t>(fb);
        }
    }
    bool sacn_changed = false;
    if (get_bool("sacn_enabled", b)) {
        sacn_changed   = (g.sacn_enabled != b);
        g.sacn_enabled = b;
    }
    bool fpp_changed = false;
    if (get_bool("fpp_remote", b)) {
        fpp_changed  = (g.fpp_remote != b);
        g.fpp_remote = b;
    }

    // Admin password: separate setter (hashes + persists on its own); empty
    // string clears it (auth off). Copied out before cJSON_Delete frees the
    // backing buffer. Never echoed back. Too long is refused, not truncated:
    // a truncated hash would never match what the browser sends back.
    char pwd[config::kMaxWebPasswordLen + 1];
    bool password_changed = false;
    if ((s = get_str("web_password"))) {
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

    // Turning the web UI off from the web UI: this server stops right after
    // the answer (it used to keep serving until a reboot).
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (network_changed)
        cJSON_AddStringToObject(resp, "note", "network_changes_apply_after_reboot");
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

    char buf[512];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");

    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");

    config::ChannelConfig c = config::get_channel(static_cast<size_t>(idx));

    auto get_u32 = [&](const char* key, uint32_t lo, uint32_t hi, uint32_t& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsNumber(item)) return false;
        // Range-check the double first: casting an out-of-range value
        // (-5, 1e40) to uint32_t is undefined (RISC-V saturates -5 to 0).
        const double d = item->valuedouble;
        if (!(d >= lo && d <= hi)) return false;
        out = static_cast<uint32_t>(d);
        return true;
    };
    auto get_bool = [&](const char* key, bool& out) -> bool {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item) return false;
        if (cJSON_IsBool(item)) {
            out = cJSON_IsTrue(item);
            return true;
        }
        if (cJSON_IsNumber(item)) {
            out = (item->valuedouble != 0);
            return true;
        }
        return false;
    };
    auto get_str = [&](const char* key) -> const char* {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(j, key);
        if (!item || !cJSON_IsString(item)) return nullptr;
        return item->valuestring;
    };

    uint32_t u = 0;
    bool b     = false;
    const char* s;

    if ((s = get_str("protocol"))) {
        const int p = lookup(kProtoNames, static_cast<size_t>(led::Protocol::COUNT), s);
        if (p < 0) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad protocol");
        }
        c.protocol = static_cast<led::Protocol>(p);
    }
    if ((s = get_str("color_order"))) {
        const int o = lookup(kOrderNames, static_cast<size_t>(led::ColorOrder::COUNT), s);
        if (o < 0) {
            cJSON_Delete(j);
            return send_err(req, 400, "bad color_order");
        }
        c.color_order = static_cast<led::ColorOrder>(o);
    }
    if (get_u32("universe_start", 0, 32767, u)) c.universe_start = static_cast<uint16_t>(u);
    if (get_u32("dmx_start", 1, 512, u)) c.dmx_start = static_cast<uint16_t>(u);
    if (get_u32("pixel_count", 1, dmx::kMaxPixelsPerChan, u))
        c.pixel_count = static_cast<uint16_t>(u);
    if (get_u32("brightness", 0, 255, u)) c.brightness = static_cast<uint8_t>(u);
    if (get_u32("grouping", 1, 8, u)) c.grouping = static_cast<uint8_t>(u);
    if (get_bool("invert", b)) c.invert_direction = b;
    if (get_u32("clock_hz", led::kMinClockHz, led::kMaxClockHz, u)) c.clock_hz = u;
    if (get_u32("gamma_x10", 10, 40, u)) c.gamma_x10 = static_cast<uint8_t>(u);
    if ((s = get_str("wb"))) {
        unsigned wr, wg, wbv;
        if (sscanf(s[0] == '#' ? s + 1 : s, "%02x%02x%02x", &wr, &wg, &wbv) == 3) {
            c.wb_r = wr ? static_cast<uint8_t>(wr) : 255;
            c.wb_g = wg ? static_cast<uint8_t>(wg) : 255;
            c.wb_b = wbv ? static_cast<uint8_t>(wbv) : 255;
        }
    }
    apply_gaps_json(j, c);

    cJSON_Delete(j);
    config::set_channel(static_cast<size_t>(idx), c);
    dmx::mark_channel_dirty(static_cast<size_t>(idx));
    return send_ok(req);
}

// ── POST /api/autopatch ─────────────────────────────────────────────────────
// Re-address every channel contiguously from a base universe (cascade by each
// channel's pixel span). Body: {"base": <0..32767>}. Replies the next free
// universe past the last channel.
esp_err_t handle_autopatch(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char buf[64];
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    cJSON* base_item = cJSON_GetObjectItemCaseSensitive(j, "base");
    const bool ok    = base_item && cJSON_IsNumber(base_item) && base_item->valuedouble >= 0 &&
                    base_item->valuedouble <= 32767;
    const uint16_t base = ok ? static_cast<uint16_t>(base_item->valuedouble) : 0;
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, "base 0..32767");

    uint16_t next = 0;
    dmx::auto_patch_universes(base, &next);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "next_free", next);
    return send_json(req, root);
}

}  // namespace pixfrog::web::impl
