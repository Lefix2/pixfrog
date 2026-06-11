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
Scene g_scenes[kNumScenes]{};
bool g_nvs_ok = false;

constexpr const char* kKeyScenes = "scenes";

GlobalConfig make_default_global() {
    GlobalConfig g{};
    g.use_dhcp      = true;
    g.artnet_net    = 0;
    g.artnet_subnet = 0;
    std::strncpy(g.short_name, "pixfrog", kArtnetNameShortMax - 1);
    std::strncpy(g.long_name, "pixfrog LED controller", kArtnetNameLongMax - 1);
    g.artnet_poll_reply_unicast = false;
    g.refresh_rate_hz           = 60;
    g.home_timeout_s            = 30;
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
    c.clock_hz         = 4'000'000;
    c.gamma_x10        = 10;   // linear
    c.wb_r             = 255;  // unity
    c.wb_g             = 255;
    c.wb_b             = 255;
    return c;
}

// Loads a blob from NVS into dst (size bytes). Handles forward migration: if
// the stored blob is smaller than size (struct grew), the tail is zero-filled
// so new fields get their safe zero default. Returns false only on hard error
// or if the stored blob is *larger* than expected (downgrade scenario).
// Usable starter set: 0-2 showcase each effect, the rest are named slots.
void fill_default_scenes() {
    std::memset(g_scenes, 0, sizeof(g_scenes));
    for (size_t i = 0; i < kNumScenes; ++i) {
        std::snprintf(g_scenes[i].name, kSceneNameMax, "Scene %u", static_cast<unsigned>(i + 1));
        g_scenes[i].channel_mask = 0xFF;
        g_scenes[i].effect       = kSceneFxSolid;
    }
    std::strncpy(g_scenes[0].name, "Warm white", kSceneNameMax - 1);
    g_scenes[0].r = 255;
    g_scenes[0].g = 180;
    g_scenes[0].b = 110;
    std::strncpy(g_scenes[1].name, "Chase", kSceneNameMax - 1);
    g_scenes[1].effect = kSceneFxChase;
    g_scenes[1].r = g_scenes[1].g = g_scenes[1].b = 255;
    g_scenes[1].speed                             = 60;  // px/s
    g_scenes[1].param                             = 3;   // head width
    std::strncpy(g_scenes[2].name, "Rainbow", kSceneNameMax - 1);
    g_scenes[2].effect = kSceneFxRainbow;
    g_scenes[2].speed  = 50;
    g_scenes[2].param  = 1;
}

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

    char key[8];
    for (size_t i = 0; i < kNumChannels; ++i) {
        channel_key(i, key);
        if (!nvs_load_blob(h, key, &g_channels[i], sizeof(ChannelConfig))) {
            g_channels[i] = make_default_channel(i);
            nvs_save_blob(h, key, &g_channels[i], sizeof(ChannelConfig));
        }
        sanitize_channel(g_channels[i]);
    }

    if (!nvs_load_blob(h, kKeyScenes, g_scenes, sizeof(g_scenes))) {
        fill_default_scenes();
        nvs_save_blob(h, kKeyScenes, g_scenes, sizeof(g_scenes));
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

const Scene& get_scene(size_t i) {
    return g_scenes[i < kNumScenes ? i : 0];
}

bool set_scene(size_t i, const Scene& scene) {
    if (i >= kNumScenes) return false;
    g_scenes[i]                         = scene;
    g_scenes[i].name[kSceneNameMax - 1] = '\0';
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_save_blob(h, kKeyScenes, g_scenes, sizeof(g_scenes));
    nvs_commit(h);
    nvs_close(h);
    return true;
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
    nvs_save_blob(h, kKeyScenes, g_scenes, sizeof(g_scenes));
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace pixfrog::config
