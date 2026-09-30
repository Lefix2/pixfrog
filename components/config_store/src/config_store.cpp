#include "config_store.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace pixfrog::config {

namespace {

constexpr const char* TAG        = "CFG";
constexpr const char* kNamespace = "pixfrog";
constexpr const char* kKeyGlobal = "global";

GlobalConfig g_global{};
ChannelConfig g_channels[kNumChannels]{};
SceneBank g_bank{};
bool g_nvs_ok = false;

constexpr const char* kKeyScenes   = "scenes";
constexpr const char* kKeyRollback = "rollback";

GlobalConfig make_default_global() {
    GlobalConfig g{};
    g.use_dhcp      = true;
    g.artnet_net    = 0;
    g.artnet_subnet = 0;
    std::strncpy(g.short_name, "pixfrog", kArtnetNameShortMax - 1);
    std::strncpy(g.long_name, "pixfrog LED controller", kArtnetNameLongMax - 1);
    g.artnet_poll_reply_unicast = false;
    g.refresh_rate_hz           = kDefaultRefreshHz;
    g.home_timeout_s            = 30;
    // Fresh installs dim the panel once idle — the ER-TFT2.79-1 backlight is
    // rated 30 000 h at its typical current. A config migrated from a
    // pre-dimming firmware zero-fills instead, keeping its always-bright look.
    g.tft_brightness  = 100;
    g.tft_idle_dim    = 60;
    g.tft_dim_delay_s = 30;
    return g;
}

ChannelConfig make_default_channel(size_t idx) {
    ChannelConfig c{};
    c.protocol         = led::Protocol::Off;  // channels start disabled; opt-in per channel
    c.color_order      = led::ColorOrder::GRB;
    c.universe_start   = static_cast<uint16_t>(1 +
                                               idx * 6);  // give each channel 6 universes by default
    c.dmx_start        = 1;
    c.pixel_count      = 144;
    c.brightness       = 255;
    c.grouping         = 1;
    c.invert_direction = false;
    c.clock_hz         = kDefaultClockHz;
    c.gamma_x10        = 10;   // linear
    c.wb_r             = 255;  // unity
    c.wb_g             = 255;
    c.wb_b             = 255;
    return c;
}

// Usable starter set: one slot per showcase effect.
void fill_default_scenes() {
    std::memset(&g_bank, 0, sizeof(g_bank));
    struct Def {
        const char* name;
        uint8_t effect, speed, param, n;
        uint8_t rgb[kSceneColorsMax][3];
    };
    static const Def kDefs[kLegacyNumScenes] = {
        { "Warm white", kSceneFxSolid, 0, 0, 1, { { 255, 180, 110 } } },
        { "Chase", kSceneFxChase, 60, 3, 2, { { 255, 255, 255 }, { 255, 120, 0 } } },
        { "Rainbow", kSceneFxRainbow, 50, 1, 1, { { 255, 255, 255 } } },
        { "Blobs", kSceneFxBlobs, 40, 4, 3, { { 0, 90, 255 }, { 255, 0, 140 }, { 0, 255, 160 } } },
        { "Fire",
          kSceneFxFire,
          60,
          0,
          4,
          { { 180, 16, 0 }, { 255, 80, 0 }, { 255, 170, 20 }, { 255, 240, 150 } } },
        { "Twinkle", kSceneFxTwinkle, 60, 60, 2, { { 255, 200, 120 }, { 160, 200, 255 } } },
        { "Scanner", kSceneFxScanner, 80, 0, 1, { { 255, 0, 0 } } },
        { "Strobe", kSceneFxSolid, 0, 0, 2, { { 0, 0, 0 }, { 255, 255, 255 } } },
    };
    g_bank.count = kLegacyNumScenes;
    for (size_t i = 0; i < kLegacyNumScenes; ++i) {
        Scene& sc    = g_bank.scenes[i];
        const Def& d = kDefs[i];
        std::strncpy(sc.name, d.name, kSceneNameMax - 1);
        sc.channel_mask = 0xFF;
        sc.effect       = d.effect;
        sc.speed        = d.speed;
        sc.param        = d.param;
        sc.num_colors   = d.n;
        for (size_t k = 0; k < d.n; ++k)
            set_scene_color(sc, k, d.rgb[k][0], d.rgb[k][1], d.rgb[k][2]);
    }
}

// Loads a blob from NVS into dst (size bytes). Handles forward migration: if
// the stored blob is smaller than size (struct grew), the tail is zero-filled
// so new fields get their safe zero default. Returns false only on hard error
// or if the stored blob is *larger* than expected (downgrade scenario).
bool nvs_load_blob(nvs_handle_t handle, const char* key, void* dst, size_t size) {
    size_t actual = 0;
    esp_err_t err = nvs_get_blob(handle, key, nullptr, &actual);
    if (err != ESP_OK) return false;
    if (actual > size) return false;  // stored blob larger than struct — reject
    std::memset(dst, 0, size);
    err = nvs_get_blob(handle, key, dst, &actual);
    return err == ESP_OK;
}

void nvs_save_blob(nvs_handle_t handle, const char* key, const void* src, size_t size) {
    esp_err_t err = nvs_set_blob(handle, key, src, size);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %d", key, err);
}

void save_scenes(nvs_handle_t h) {
    nvs_save_blob(h, kKeyScenes, &g_bank, scene_bank_bytes(g_bank.count));
}

// Persists the scene list (and the global config when a structural edit moved
// its scene references). RAM is already updated; false = not persisted.
bool persist_scenes(bool with_global) {
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    save_scenes(h);
    if (with_global) nvs_save_blob(h, kKeyGlobal, &g_global, sizeof(g_global));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

void sanitize_scene(Scene& sc) {
    sc.name[kSceneNameMax - 1] = '\0';
    sc.num_colors              = scene_num_colors(sc);
    if (sc.effect >= kSceneFxCount) sc.effect = kSceneFxSolid;
}

// Follows boot/failsafe references through a list edit. Returns true when
// either changed (the global config then needs persisting too).
bool remap_global_scene_refs(SceneEdit op, size_t a, size_t b) {
    const int boot      = remap_scene_index(static_cast<int>(g_global.boot_scene) - 1, op, a, b);
    const auto nb       = static_cast<uint8_t>(boot < 0 ? 0 : boot + 1);
    bool changed        = nb != g_global.boot_scene;
    g_global.boot_scene = nb;
    const int fs        = remap_scene_index(g_global.failsafe_scene, op, a, b);
    if (fs < 0) {
        g_global.failsafe_scene = 0;
        if (g_global.failsafe_mode == kFailsafeScene) g_global.failsafe_mode = kFailsafeBlackout;
        changed = true;
    } else if (fs != g_global.failsafe_scene) {
        g_global.failsafe_scene = static_cast<uint8_t>(fs);
        changed                 = true;
    }
    return changed;
}

void channel_key(size_t idx, char buf[8]) {
    buf[0] = 'c';
    buf[1] = 'h';
    buf[2] = static_cast<char>('0' + idx);
    buf[3] = '\0';
}

}  // namespace

namespace {

// Try a hard reset of the NVS partition (erase + re-init). Returns true if
// it succeeds. Used both at boot (to recover from any first-time failure)
// and as a last-ditch repair when nvs_open keeps failing.
bool nvs_hard_reset() {
    ESP_LOGW(TAG, "performing nvs_flash_erase + reinit");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init after erase failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void fill_ram_defaults() {
    g_global = make_default_global();
    for (size_t i = 0; i < kNumChannels; ++i)
        g_channels[i] = make_default_channel(i);
    fill_default_scenes();
}

}  // namespace

void init() {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // Any first-init error → erase + retry (covers NO_FREE_PAGES,
        // NEW_VERSION_FOUND, partition corruption, etc.).
        if (!nvs_hard_reset()) {
            ESP_LOGE(TAG, "NVS unrecoverable at boot — running with hard-coded "
                          "defaults; config changes WILL NOT PERSIST. "
                          "Check the partition table and `factory_test_nvs.bin`.");
            fill_ram_defaults();
            g_nvs_ok = false;
            return;
        }
    }

    nvs_handle_t h;
    err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        // Namespace can't be opened — perform a hard reset and try once more.
        ESP_LOGW(TAG, "nvs_open(%s) failed (%s); attempting recovery", kNamespace,
                 esp_err_to_name(err));
        if (nvs_hard_reset()) {
            err = nvs_open(kNamespace, NVS_READWRITE, &h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "NVS namespace still not openable (%s) after erase — "
                     "running with hard-coded defaults; config changes "
                     "WILL NOT PERSIST.",
                     esp_err_to_name(err));
            fill_ram_defaults();
            g_nvs_ok = false;
            return;
        }
    }

    {
        size_t stored_size = 0;
        const bool exists  = nvs_get_blob(h, kKeyGlobal, nullptr, &stored_size) == ESP_OK;
        if (!exists || !nvs_load_blob(h, kKeyGlobal, &g_global, sizeof(g_global))) {
            g_global = make_default_global();
            nvs_save_blob(h, kKeyGlobal, &g_global, sizeof(g_global));
        } else if (stored_size < sizeof(g_global)) {
            // Struct grew (firmware upgrade): persist the zero-filled tail now.
            nvs_save_blob(h, kKeyGlobal, &g_global, sizeof(g_global));
            ESP_LOGI(TAG, "global config migrated (%u→%u bytes)",
                     static_cast<unsigned>(stored_size), static_cast<unsigned>(sizeof(g_global)));
        }
    }

    if (g_global.refresh_rate_hz < kMinRefreshHz || g_global.refresh_rate_hz > kMaxRefreshHz) {
        g_global.refresh_rate_hz = kDefaultRefreshHz;
        nvs_save_blob(h, kKeyGlobal, &g_global, sizeof(g_global));
    }

    char key[8];
    for (size_t i = 0; i < kNumChannels; ++i) {
        channel_key(i, key);
        if (!nvs_load_blob(h, key, &g_channels[i], sizeof(ChannelConfig))) {
            g_channels[i] = make_default_channel(i);
            nvs_save_blob(h, key, &g_channels[i], sizeof(ChannelConfig));
        }
        sanitize_channel(g_channels[i]);
    }

    {
        size_t size      = 0;
        const bool exist = nvs_get_blob(h, kKeyScenes, nullptr, &size) == ESP_OK;
        // v1/v2 images are smaller than the bank; anything larger is unknown.
        static uint8_t raw[sizeof(SceneBank)];
        size_t n = sizeof(raw);
        if (exist && size <= sizeof(raw) && nvs_get_blob(h, kKeyScenes, raw, &n) == ESP_OK &&
            load_scene_bank(raw, n, g_bank)) {
            if (n != scene_bank_bytes(g_bank.count)) {
                save_scenes(h);
                ESP_LOGI(TAG, "scene list migrated (%u→%u bytes)", static_cast<unsigned>(n),
                         static_cast<unsigned>(scene_bank_bytes(g_bank.count)));
            }
        } else {
            fill_default_scenes();
            save_scenes(h);
        }
    }

    nvs_commit(h);
    nvs_close(h);
    g_nvs_ok = true;
    ESP_LOGI(TAG, "loaded %u channels + global config from NVS",
             static_cast<unsigned>(kNumChannels));
}

bool is_persistence_ok() {
    return g_nvs_ok;
}

bool get_rollback(RollbackRecord& out) {
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n       = sizeof(out);
    const bool got = nvs_get_blob(h, kKeyRollback, &out, &n) == ESP_OK && n == sizeof(out);
    nvs_close(h);
    if (got) {
        out.rejected_version[sizeof(out.rejected_version) - 1] = '\0';
        out.rejected_slot[sizeof(out.rejected_slot) - 1]       = '\0';
        out.running_version[sizeof(out.running_version) - 1]   = '\0';
    }
    return got;
}

bool set_rollback(const RollbackRecord& rec) {
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_save_blob(h, kKeyRollback, &rec, sizeof(rec));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

const GlobalConfig& get_global() {
    return g_global;
}
const ChannelConfig& get_channel(size_t i) {
    return g_channels[i];
}

bool set_global(const GlobalConfig& cfg) {
    g_global = cfg;
    if (!g_nvs_ok) return false;  // RAM-only: cache updated, no persistence
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_save_blob(h, kKeyGlobal, &g_global, sizeof(g_global));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

bool set_channel(size_t i, const ChannelConfig& cfg) {
    if (i >= kNumChannels) return false;
    g_channels[i] = cfg;
    sanitize_channel(g_channels[i]);
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    char key[8];
    channel_key(i, key);
    nvs_save_blob(h, key, &g_channels[i], sizeof(ChannelConfig));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

namespace {

void web_password_hash(const uint8_t salt[8], const char* password, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // SHA-256 (not 224)
    mbedtls_sha256_update(&ctx, salt, 8);
    mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(password),
                          std::strlen(password));
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

bool hash_is_zero(const uint8_t hash[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i)
        acc |= hash[i];
    return acc == 0;
}

}  // namespace

bool set_web_password(const char* password) {
    GlobalConfig g = g_global;
    if (!password || password[0] == '\0') {
        std::memset(g.web_auth_salt, 0, sizeof(g.web_auth_salt));
        std::memset(g.web_auth_hash, 0, sizeof(g.web_auth_hash));
    } else {
        esp_fill_random(g.web_auth_salt, sizeof(g.web_auth_salt));
        web_password_hash(g.web_auth_salt, password, g.web_auth_hash);
    }
    return set_global(g);
}

bool web_password_set() {
    return !hash_is_zero(g_global.web_auth_hash);
}

bool check_web_password(const char* password) {
    if (!web_password_set()) return true;  // auth disabled
    if (!password) return false;
    uint8_t candidate[32];
    web_password_hash(g_global.web_auth_salt, password, candidate);
    // Constant-time compare: no early exit on mismatch.
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i)
        diff |= candidate[i] ^ g_global.web_auth_hash[i];
    return diff == 0;
}

size_t num_scenes() {
    return g_bank.count;
}

const Scene& get_scene(size_t i) {
    static const Scene kBlank{};
    return i < g_bank.count ? g_bank.scenes[i] : kBlank;
}

bool set_scene(size_t i, const Scene& scene) {
    if (i >= g_bank.count) return false;
    g_bank.scenes[i] = scene;
    sanitize_scene(g_bank.scenes[i]);
    return persist_scenes(false);
}

int add_scene(const Scene& scene) {
    if (g_bank.count >= kMaxScenes) return -1;
    Scene& sc = g_bank.scenes[g_bank.count];
    sc        = scene;
    sanitize_scene(sc);
    const int idx = g_bank.count++;
    persist_scenes(false);
    return idx;
}

bool delete_scene(size_t i) {
    if (i >= g_bank.count) return false;
    std::memmove(&g_bank.scenes[i], &g_bank.scenes[i + 1], (g_bank.count - i - 1) * sizeof(Scene));
    --g_bank.count;
    std::memset(&g_bank.scenes[g_bank.count], 0, sizeof(Scene));
    persist_scenes(remap_global_scene_refs(SceneEdit::Delete, i, 0));
    return true;
}

bool move_scene(size_t from, size_t to) {
    if (from >= g_bank.count || to >= g_bank.count) return false;
    if (from == to) return true;
    const Scene moved = g_bank.scenes[from];
    if (from < to)
        std::memmove(&g_bank.scenes[from], &g_bank.scenes[from + 1], (to - from) * sizeof(Scene));
    else
        std::memmove(&g_bank.scenes[to + 1], &g_bank.scenes[to], (from - to) * sizeof(Scene));
    g_bank.scenes[to] = moved;
    persist_scenes(remap_global_scene_refs(SceneEdit::Move, from, to));
    return true;
}

bool replace_scenes(const Scene* scenes, size_t count) {
    if (count > kMaxScenes) count = kMaxScenes;
    std::memset(&g_bank, 0, sizeof(g_bank));
    for (size_t i = 0; i < count; ++i) {
        g_bank.scenes[i] = scenes[i];
        sanitize_scene(g_bank.scenes[i]);
    }
    g_bank.count = static_cast<uint8_t>(count);
    return persist_scenes(false);
}

void reset_to_defaults() {
    fill_ram_defaults();
    if (!g_nvs_ok) return;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_save_blob(h, kKeyGlobal, &g_global, sizeof(g_global));
    char key[8];
    for (size_t i = 0; i < kNumChannels; ++i) {
        channel_key(i, key);
        nvs_save_blob(h, key, &g_channels[i], sizeof(ChannelConfig));
    }
    save_scenes(h);
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace pixfrog::config
