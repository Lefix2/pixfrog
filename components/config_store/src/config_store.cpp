#include "config_store.h"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace pixfrog::config {

namespace {

constexpr const char* TAG = "CFG";
constexpr const char* kNamespace = "pixfrog";
constexpr const char* kKeyGlobal = "global";

GlobalConfig  g_global{};
ChannelConfig g_channels[kNumChannels]{};
bool          g_nvs_ok = false;

GlobalConfig make_default_global() {
    GlobalConfig g{};
    g.use_dhcp        = true;
    g.artnet_net      = 0;
    g.artnet_subnet   = 0;
    std::strncpy(g.short_name, "pixfrog",        kArtnetNameShortMax - 1);
    std::strncpy(g.long_name,  "pixfrog LED controller", kArtnetNameLongMax - 1);
    g.artnet_poll_reply_unicast = false;
    g.refresh_rate_hz = 60;
    g.home_timeout_s  = 30;
    return g;
}

ChannelConfig make_default_channel(size_t idx) {
    ChannelConfig c{};
    c.protocol         = led::Protocol::WS2815;
    c.color_order      = led::ColorOrder::GRB;
    c.universe_start   = static_cast<uint16_t>(1 + idx * 6);    // give each channel 6 universes by default
    c.dmx_start        = 1;
    c.pixel_count      = 144;
    c.brightness       = 255;
    c.grouping         = 1;
    c.invert_direction = false;
    c.clock_hz         = 4'000'000;
    return c;
}

bool nvs_load_blob(nvs_handle_t handle, const char* key, void* dst, size_t size) {
    size_t actual = size;
    esp_err_t err = nvs_get_blob(handle, key, dst, &actual);
    return err == ESP_OK && actual == size;
}

void nvs_save_blob(nvs_handle_t handle, const char* key, const void* src, size_t size) {
    esp_err_t err = nvs_set_blob(handle, key, src, size);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_blob(%s) failed: %d", key, err);
}

void channel_key(size_t idx, char buf[8]) {
    buf[0] = 'c'; buf[1] = 'h'; buf[2] = static_cast<char>('0' + idx); buf[3] = '\0';
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
    for (size_t i = 0; i < kNumChannels; ++i) g_channels[i] = make_default_channel(i);
}

}  // namespace

void init() {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // Any first-init error → erase + retry (covers NO_FREE_PAGES,
        // NEW_VERSION_FOUND, partition corruption, etc.).
        if (!nvs_hard_reset()) {
            ESP_LOGE(TAG,
                     "NVS unrecoverable at boot — running with hard-coded "
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
        ESP_LOGW(TAG, "nvs_open(%s) failed (%s); attempting recovery",
                 kNamespace, esp_err_to_name(err));
        if (nvs_hard_reset()) {
            err = nvs_open(kNamespace, NVS_READWRITE, &h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "NVS namespace still not openable (%s) after erase — "
                     "running with hard-coded defaults; config changes "
                     "WILL NOT PERSIST.", esp_err_to_name(err));
            fill_ram_defaults();
            g_nvs_ok = false;
            return;
        }
    }

    if (!nvs_load_blob(h, kKeyGlobal, &g_global, sizeof(g_global))) {
        g_global = make_default_global();
        nvs_save_blob(h, kKeyGlobal, &g_global, sizeof(g_global));
    }

    char key[8];
    for (size_t i = 0; i < kNumChannels; ++i) {
        channel_key(i, key);
        if (!nvs_load_blob(h, key, &g_channels[i], sizeof(ChannelConfig))) {
            g_channels[i] = make_default_channel(i);
            nvs_save_blob(h, key, &g_channels[i], sizeof(ChannelConfig));
        }
    }

    nvs_commit(h);
    nvs_close(h);
    g_nvs_ok = true;
    ESP_LOGI(TAG, "loaded %u channels + global config from NVS",
             static_cast<unsigned>(kNumChannels));
}

bool is_persistence_ok() { return g_nvs_ok; }

const GlobalConfig&  get_global()                          { return g_global; }
const ChannelConfig& get_channel(size_t i)                 { return g_channels[i]; }

bool set_global(const GlobalConfig& cfg) {
    g_global = cfg;
    if (!g_nvs_ok) return false;   // RAM-only: cache updated, no persistence
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
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace pixfrog::config
