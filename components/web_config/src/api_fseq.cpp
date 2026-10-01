// FSEQ API: files, play/stop, upload, playlist.

#include "web_internal.h"

namespace pixfrog::web::impl {

// ── FSEQ playlist JSON (GET/POST /api/fseq/playlist, config, backup) ───────

static const char* fseq_name_error(const char* name);

cJSON* build_playlist_json() {
    const config::FseqPlaylist& p = config::get_playlist();
    cJSON* root                   = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "loop", p.loop);
    cJSON_AddBoolToObject(root, "autostart", p.autostart);
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < p.count; ++i) {
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", p.items[i].name);
        cJSON_AddNumberToObject(it, "repeat", p.items[i].repeat);
        cJSON_AddItemToArray(items, it);
    }
    return root;
}

// Applies `j` onto `p` (fields absent keep their value; `items` replaces the
// whole list). False with *why on the first invalid field, `p` then partial.
bool apply_playlist_json(const cJSON* j, config::FseqPlaylist& p, const char** why) {
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(j, "loop");
    if (it) {
        if (!cJSON_IsBool(it)) return *why = "loop: true|false", false;
        p.loop = cJSON_IsTrue(it) ? 1 : 0;
    }
    it = cJSON_GetObjectItemCaseSensitive(j, "autostart");
    if (it) {
        if (!cJSON_IsBool(it)) return *why = "autostart: true|false", false;
        p.autostart = cJSON_IsTrue(it) ? 1 : 0;
    }
    const cJSON* items = cJSON_GetObjectItemCaseSensitive(j, "items");
    if (!items) return true;
    if (!cJSON_IsArray(items)) return *why = "items: array", false;
    if (cJSON_GetArraySize(items) > static_cast<int>(config::kPlaylistMax))
        return *why = "items: 16 at most", false;
    config::FseqPlaylist out = p;
    out.count                = 0;
    for (const cJSON* e = items->child; e; e = e->next) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(e, "name");
        if (!cJSON_IsString(name) || !name->valuestring)
            return *why = "items[].name: string", false;
        if (const char* bad = fseq_name_error(name->valuestring)) return *why = bad, false;
        uint8_t repeat   = 1;
        const cJSON* rep = cJSON_GetObjectItemCaseSensitive(e, "repeat");
        if (rep) {
            if (!cJSON_IsNumber(rep) || !(rep->valuedouble >= 1 && rep->valuedouble <= 255))
                return *why = "items[].repeat: 1..255", false;
            repeat = static_cast<uint8_t>(rep->valuedouble);
        }
        config::PlaylistItem& dst = out.items[out.count++];
        dst                       = config::PlaylistItem{};
        std::strncpy(dst.name, name->valuestring, sizeof(dst.name) - 1);
        dst.repeat = repeat;
    }
    for (size_t i = out.count; i < config::kPlaylistMax; ++i)
        out.items[i] = config::PlaylistItem{};
    p = out;
    return true;
}

// ── GET /api/fseq/files ──────────────────────────────────────────────────────

esp_err_t handle_fseq_files(httpd_req_t* req) {
    char names[fseq::kMaxFiles][fseq::kMaxNameLen];
    const size_t n = fseq::list_files(names, fseq::kMaxFiles);
    cJSON* arr     = cJSON_CreateArray();
    for (size_t i = 0; i < n; ++i)
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "files", arr);
    const char* playing = fseq::active_file();
    cJSON_AddStringToObject(root, "active", playing ? playing : "");
    return send_json(req, root);
}

// ── POST /api/fseq/play ──────────────────────────────────────────────────────

// A file name the card may be asked for: a plain `name.fseq` in the mount
// root — no path separator, no hidden/relative name, no silent truncation.
// nullptr when acceptable, else the reason.
static const char* fseq_name_error(const char* name) {
    if (!name[0]) return "missing filename";
    if (strchr(name, '/') || strchr(name, '\\') || name[0] == '.') return "bad filename";
    const size_t nl = strlen(name);
    if (nl >= fseq::kMaxNameLen) return "filename too long";
    if (nl < 6 || strcasecmp(name + nl - 5, ".fseq") != 0) return "filename must end in .fseq";
    return nullptr;
}

esp_err_t handle_fseq_play(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    char body[fseq::kMaxNameLen + 64];
    if (!read_body(req, body, sizeof(body) - 1))
        return send_err(req, 400, "body too large or empty");
    cJSON* root = cJSON_Parse(body);
    if (!root) return send_err(req, 400, "Invalid JSON");
    // {"playlist":true} plays the stored playlist instead of one file.
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "playlist"))) {
        cJSON_Delete(root);
        if (!fseq::start_playlist()) return send_err(req, 409, fseq::error_string());
        return send_ok(req);
    }
    const cJSON* jloop = cJSON_GetObjectItemCaseSensitive(root, "loop");
    if (jloop && !cJSON_IsBool(jloop)) {
        cJSON_Delete(root);
        return send_err(req, 400, "loop: true|false");
    }
    const bool loop = cJSON_IsTrue(jloop);
    cJSON* fn       = cJSON_GetObjectItemCaseSensitive(root, "filename");
    if (!cJSON_IsString(fn) || !fn->valuestring) {
        cJSON_Delete(root);
        return send_err(req, 400, "missing filename");
    }
    const char* why = fseq_name_error(fn->valuestring);
    if (why) {
        cJSON_Delete(root);
        return send_err(req, 400, why);
    }
    char filename[fseq::kMaxNameLen];
    strncpy(filename, fn->valuestring, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    cJSON_Delete(root);
    if (!fseq::start(filename, loop)) return send_err(req, 500, fseq::error_string());
    return send_ok(req);
}

// ── GET|POST /api/fseq/playlist ──────────────────────────────────────────────
// POST takes {loop?, autostart?, items?:[{name, repeat?}]}; items replaces the
// list. The playing playlist keeps its snapshot until restarted.

esp_err_t handle_get_playlist(httpd_req_t* req) {
    return send_json(req, build_playlist_json());
}

esp_err_t handle_post_playlist(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    static char buf[4096];  // 16 items × (64-byte name + JSON) — off the httpd stack
    if (!read_body(req, buf, sizeof(buf) - 1)) return send_err(req, 400, "body too large or empty");
    cJSON* j = cJSON_Parse(buf);
    if (!j) return send_err(req, 400, "invalid JSON");
    config::FseqPlaylist p = config::get_playlist();
    const char* why        = nullptr;
    const bool ok          = cJSON_IsObject(j) && apply_playlist_json(j, p, &why);
    cJSON_Delete(j);
    if (!ok) return send_err(req, 400, why ? why : "expected an object");
    config::set_playlist(p);
    return send_json(req, build_playlist_json());
}

// ── POST /api/fseq/stop ──────────────────────────────────────────────────────

esp_err_t handle_fseq_stop(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    fseq::stop();
    return send_ok(req);
}

// ── POST /api/fseq/upload?name=<file.fseq> ──────────────────────────────────
// Raw body streamed to the SD card. The data lands in a .part temp file
// first and is renamed on success, so an interrupted upload never leaves a
// truncated .fseq visible to the player.

static std::atomic<uint32_t> g_fseq_upload_in_progress{ 0 };  // RMW: 32-bit

// Decode %XX sequences in-place (httpd_query_key_value does not URL-decode).
static void url_decode(char* s) {
    char* w = s;
    for (; *s; ++w) {
        if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3]  = { s[1], s[2], 0 };
            *w           = static_cast<char>(strtol(hex, nullptr, 16));
            s           += 3;
        } else {
            *w = (*s == '+') ? ' ' : *s;
            ++s;
        }
    }
    *w = '\0';
}

esp_err_t handle_fseq_upload(httpd_req_t* req) {
    if (!require_auth(req)) return ESP_OK;
    if (g_fseq_upload_in_progress.exchange(1))
        return send_err(req, 500, "upload already in progress");
    // Claimed above — every exit from here on must hand the flag back.
    const auto reject = [req](int code, const char* msg) {
        g_fseq_upload_in_progress = false;
        return send_err(req, code, msg);
    };

    if (fseq::sd_state() != fseq::SdState::Mounted) return reject(500, "no SD card");

    char query[fseq::kMaxNameLen * 3 + 16] = {};
    char name[fseq::kMaxNameLen]           = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || !name[0])
        return reject(400, "missing ?name=<file.fseq>");
    url_decode(name);

    if (const char* why = fseq_name_error(name)) return reject(400, why);

    const int total = req->content_len;
    if (total <= 0) return reject(400, "empty body");

    // Overwriting the file currently playing: stop playback to release it.
    const char* active = fseq::active_file();
    if (active && strcasecmp(active, name) == 0) fseq::stop();

    char tmp_path[96];
    char path[96 + fseq::kMaxNameLen];
    snprintf(tmp_path, sizeof(tmp_path), "%s/upload.part", fseq::kMountPath);
    snprintf(path, sizeof(path), "%s/%s", fseq::kMountPath, name);

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) return reject(500, "cannot create file on SD");
    ESP_LOGI(TAG, "FSEQ upload: %d bytes -> %s", total, name);

    // Static buffer: the httpd task stack is small and only one upload can
    // run at a time (guarded by g_fseq_upload_in_progress).
    static char buf[4096];
    int remaining = total;
    while (remaining > 0) {
        const int n = httpd_req_recv(
            req, buf,
            remaining < static_cast<int>(sizeof(buf)) ? remaining : static_cast<int>(sizeof(buf)));
        if (n <= 0 || fwrite(buf, 1, n, fp) != static_cast<size_t>(n)) {
            fclose(fp);
            remove(tmp_path);
            g_fseq_upload_in_progress = false;
            return send_err(req, n <= 0 ? 400 : 500,
                            n <= 0 ? "upload interrupted" : "SD write failed (card full?)");
        }
        remaining -= n;
    }
    fclose(fp);

    remove(path);  // FATFS rename does not overwrite
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        g_fseq_upload_in_progress = false;
        return send_err(req, 500, "rename failed");
    }
    g_fseq_upload_in_progress = false;
    ESP_LOGI(TAG, "FSEQ upload complete: %s", name);
    return send_ok(req);
}

}  // namespace pixfrog::web::impl
