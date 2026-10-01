#include "config_store.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
ControlConfig g_control{};
FseqPlaylist g_playlist{};
bool g_nvs_ok = false;

// The config lock (see ScopedLock). Created by init(); before that (and if
// creation failed) locking is a no-op — boot is single-threaded until then.
SemaphoreHandle_t g_mux = nullptr;

// Seqlock over the scene bank's RAM image, for copy_scene(): odd while an edit
// is in flight. Writers already hold the config lock; 32-bit for the P4.
std::atomic<uint32_t> g_bank_seq{ 0 };
struct BankEdit {
    BankEdit() { g_bank_seq.fetch_add(1, std::memory_order_acq_rel); }
    ~BankEdit() { g_bank_seq.fetch_add(1, std::memory_order_release); }
    BankEdit(const BankEdit&)            = delete;
    BankEdit& operator=(const BankEdit&) = delete;
};

constexpr const char* kKeyScenes   = "scenes";
constexpr const char* kKeyRollback = "rollback";
constexpr const char* kKeyControl  = "control";
constexpr const char* kKeyPlaylist = "playlist";

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
    BankEdit edit;
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
// Layout version of each blob, kept in its own one-byte "v_<key>" entry.
// Bump a version when a struct's *layout* changes (fields moved, resized):
// the loader can then tell old and new images apart without relying on
// their sizes never colliding. A struct that only grows at the end keeps its
// version — the shorter image loads zero-filled. An entry written before
// versioning has no "v_" key: its layout is inferred from its size, as
// before, and the next save records the version.
uint8_t layout_version(const char* key) {
    if (std::strcmp(key, kKeyScenes) == 0) return 3;  // v1 8×25 B, v2 8×34 B, v3 count+N×34 B
    return 1;                                         // global, ch*, control, playlist, rollback
}

void version_key(const char* key, char out[16]) {
    std::snprintf(out, 16, "v_%s", key);
}

// The stored layout version of `key`, -1 when none was recorded.
int stored_version(nvs_handle_t handle, const char* key) {
    char vk[16];
    version_key(key, vk);
    uint8_t v = 0;
    size_t n  = 1;
    if (nvs_get_blob(handle, vk, &v, &n) != ESP_OK || n != 1) return -1;
    return v;
}

bool nvs_load_blob(nvs_handle_t handle, const char* key, void* dst, size_t size) {
    // Written by a newer firmware with another layout (a downgrade): ignore it
    // rather than misread it — the caller falls back to its defaults.
    if (stored_version(handle, key) > layout_version(key)) {
        ESP_LOGW(TAG, "%s: layout v%d is newer than this firmware's v%u — defaults", key,
                 stored_version(handle, key), layout_version(key));
        return false;
    }
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
    char vk[16];
    version_key(key, vk);
    const uint8_t ver = layout_version(key);
    if (stored_version(handle, key) != ver) nvs_set_blob(handle, vk, &ver, 1);
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
    g_control  = default_control();
    g_playlist = FseqPlaylist{};
    g_global   = make_default_global();
    for (size_t i = 0; i < kNumChannels; ++i)
        g_channels[i] = make_default_channel(i);
    fill_default_scenes();
}

}  // namespace

void init() {
    if (!g_mux) g_mux = xSemaphoreCreateRecursiveMutex();
    ScopedLock lock;
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
        // A layout newer than this firmware (a downgrade) is not read. An
        // older or unrecorded one goes through the size-based migration: a
        // downgrade to a pre-versioning firmware rewrites the blob but not
        // its "v_" entry, so the recorded version alone cannot be trusted.
        const int ver    = stored_version(h, kKeyScenes);
        const bool known = ver <= static_cast<int>(layout_version(kKeyScenes));
        if (exist && known && size <= sizeof(raw) &&
            nvs_get_blob(h, kKeyScenes, raw, &n) == ESP_OK && load_scene_bank(raw, n, g_bank)) {
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

    // Control universe: absent on a first boot or an upgrade — the default is
    // disabled, so nothing changes until the user turns it on.
    if (!nvs_load_blob(h, kKeyControl, &g_control, sizeof(g_control))) {
        g_control = default_control();
        nvs_save_blob(h, kKeyControl, &g_control, sizeof(g_control));
    }
    sanitize_control(g_control);
    if (g_global.scene_fade_ms > kMaxSceneFadeMs) g_global.scene_fade_ms = kMaxSceneFadeMs;
    if (g_global.ip_fallback > kIpFallbackArtnet) g_global.ip_fallback = kIpFallbackLinkLocal;

    // FSEQ playlist: absent before it existed — empty, nothing autostarts.
    if (!nvs_load_blob(h, kKeyPlaylist, &g_playlist, sizeof(g_playlist)))
        g_playlist = FseqPlaylist{};
    sanitize_playlist(g_playlist);

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
    ScopedLock lock;
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_save_blob(h, kKeyRollback, &rec, sizeof(rec));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

ScopedLock::ScopedLock() {
    if (g_mux) xSemaphoreTakeRecursive(g_mux, portMAX_DELAY);
}
ScopedLock::~ScopedLock() {
    if (g_mux) xSemaphoreGiveRecursive(g_mux);
}

const GlobalConfig& get_global() {
    return g_global;
}
const ChannelConfig& get_channel(size_t i) {
    return g_channels[i];
}

bool set_global(const GlobalConfig& cfg) {
    ScopedLock lock;
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
    ScopedLock lock;
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

void sha256(const uint8_t* a, size_t alen, const uint8_t* b, size_t blen, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // SHA-256 (not 224)
    mbedtls_sha256_update(&ctx, a, alen);
    if (blen) mbedtls_sha256_update(&ctx, b, blen);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

// HMAC-SHA256 (RFC 2104) over the SHA-256 already in use — no extra mbedtls
// module to enable.
void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                 uint8_t out[32]) {
    uint8_t k[64] = {};
    if (key_len > sizeof(k))
        sha256(key, key_len, nullptr, 0, k);
    else
        std::memcpy(k, key, key_len);
    uint8_t pad[64];
    for (size_t i = 0; i < 64; ++i)
        pad[i] = k[i] ^ 0x36;
    uint8_t inner[32];
    sha256(pad, sizeof(pad), msg, msg_len, inner);
    for (size_t i = 0; i < 64; ++i)
        pad[i] = k[i] ^ 0x5c;
    sha256(pad, sizeof(pad), inner, sizeof(inner), out);
}

void legacy_hash(const uint8_t salt[8], const char* password, uint8_t out[32]) {
    sha256(salt, 8, reinterpret_cast<const uint8_t*>(password), std::strlen(password), out);
}

void kdf_hash(const uint8_t salt[8], const char* password, uint8_t out[32]) {
    pbkdf2_sha256(reinterpret_cast<const uint8_t*>(password), std::strlen(password), salt, 8,
                  kWebAuthIterations, out);
}

bool hash_is_zero(const uint8_t hash[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i)
        acc |= hash[i];
    return acc == 0;
}

}  // namespace

void pbkdf2_sha256(const uint8_t* password, size_t password_len, const uint8_t* salt,
                   size_t salt_len, uint32_t iterations, uint8_t out[32]) {
    // T1 = U1 ^ U2 ^ … ^ Uc, U1 = HMAC(P, S || INT(1)), Ui = HMAC(P, Ui-1).
    uint8_t block[64 + 4];
    const size_t sl = salt_len > 64 ? 64 : salt_len;
    std::memcpy(block, salt, sl);
    block[sl]     = 0;
    block[sl + 1] = 0;
    block[sl + 2] = 0;
    block[sl + 3] = 1;
    uint8_t u[32];
    hmac_sha256(password, password_len, block, sl + 4, u);
    std::memcpy(out, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
        hmac_sha256(password, password_len, u, sizeof(u), u);
        for (size_t k = 0; k < 32; ++k)
            out[k] ^= u[k];
    }
}

bool set_web_password(const char* password) {
    ScopedLock lock;
    if (password && std::strlen(password) > kMaxWebPasswordLen) return false;
    GlobalConfig g = g_global;
    if (!password || password[0] == '\0') {
        std::memset(g.web_auth_salt, 0, sizeof(g.web_auth_salt));
        std::memset(g.web_auth_hash, 0, sizeof(g.web_auth_hash));
        g.web_auth_kdf = kWebAuthSha256;
    } else {
        esp_fill_random(g.web_auth_salt, sizeof(g.web_auth_salt));
        kdf_hash(g.web_auth_salt, password, g.web_auth_hash);
        g.web_auth_kdf = kWebAuthPbkdf2;
    }
    return set_global(g);
}

bool web_password_set() {
    return !hash_is_zero(g_global.web_auth_hash);
}

namespace {
// Basic auth re-sends the password with every request and PBKDF2 costs
// ~220 ms on the P4, so a fader would crawl. The last accepted password is
// remembered in RAM as a fast salted SHA-256, tied to the stored hash it was
// checked against (a new password, a restore or a clear drop it). A wrong
// guess never matches it and still pays the full KDF.
uint8_t g_auth_memo[32];
uint8_t g_auth_memo_for[32];  // web_auth_hash the memo was accepted under
bool g_auth_memo_valid = false;

bool same32(const uint8_t* a, const uint8_t* b) {  // constant time
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
}  // namespace

bool check_web_password(const char* password) {
    ScopedLock lock;
    if (!web_password_set()) return true;  // auth disabled
    if (!password || std::strlen(password) > kMaxWebPasswordLen) return false;
    const bool legacy = g_global.web_auth_kdf == kWebAuthSha256;
    uint8_t fast[32];
    legacy_hash(g_global.web_auth_salt, password, fast);
    if (!legacy && g_auth_memo_valid && same32(g_auth_memo_for, g_global.web_auth_hash) &&
        same32(g_auth_memo, fast))
        return true;
    uint8_t candidate[32];
    if (legacy)
        std::memcpy(candidate, fast, sizeof(candidate));
    else
        kdf_hash(g_global.web_auth_salt, password, candidate);
    if (!same32(candidate, g_global.web_auth_hash)) return false;
    // Right password, weak hash (set by an older firmware): store the KDF one.
    if (legacy) set_web_password(password);
    legacy_hash(g_global.web_auth_salt, password, g_auth_memo);  // salt may be new
    std::memcpy(g_auth_memo_for, g_global.web_auth_hash, sizeof(g_auth_memo_for));
    g_auth_memo_valid = true;
    return true;
}

size_t num_scenes() {
    return g_bank.count;
}

const Scene& get_scene(size_t i) {
    static const Scene kBlank{};
    return i < g_bank.count ? g_bank.scenes[i] : kBlank;
}

// Lock-free for the render path: a config write (NVS included) can take tens
// of ms, a frame cannot wait for it. Retry while a bank edit is in flight; if
// a writer keeps getting in the way, wait on the lock (priority inheritance).
bool copy_scene(size_t i, Scene& out) {
    for (int tries = 0; tries < 64; ++tries) {
        const uint32_t before = g_bank_seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        const bool ok   = i < g_bank.count;
        const Scene tmp = ok ? g_bank.scenes[i] : Scene{};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_bank_seq.load(std::memory_order_relaxed) == before) {
            out = tmp;
            return ok;
        }
    }
    ScopedLock lock;
    const bool ok = i < g_bank.count;
    out           = ok ? g_bank.scenes[i] : Scene{};
    return ok;
}

bool set_scene(size_t i, const Scene& scene) {
    ScopedLock lock;
    if (i >= g_bank.count) return false;
    {
        BankEdit edit;
        g_bank.scenes[i] = scene;
        sanitize_scene(g_bank.scenes[i]);
    }
    return persist_scenes(false);
}

int add_scene(const Scene& scene) {
    ScopedLock lock;
    if (g_bank.count >= kMaxScenes) return -1;
    int idx;
    {
        BankEdit edit;
        Scene& sc = g_bank.scenes[g_bank.count];
        sc        = scene;
        sanitize_scene(sc);
        idx = g_bank.count++;
    }
    persist_scenes(false);
    return idx;
}

bool delete_scene(size_t i) {
    ScopedLock lock;
    if (i >= g_bank.count) return false;
    {
        BankEdit edit;
        std::memmove(&g_bank.scenes[i], &g_bank.scenes[i + 1],
                     (g_bank.count - i - 1) * sizeof(Scene));
        --g_bank.count;
        std::memset(&g_bank.scenes[g_bank.count], 0, sizeof(Scene));
    }
    persist_scenes(remap_global_scene_refs(SceneEdit::Delete, i, 0));
    return true;
}

bool move_scene(size_t from, size_t to) {
    ScopedLock lock;
    if (from >= g_bank.count || to >= g_bank.count) return false;
    if (from == to) return true;
    {
        BankEdit edit;
        const Scene moved = g_bank.scenes[from];
        if (from < to)
            std::memmove(&g_bank.scenes[from], &g_bank.scenes[from + 1],
                         (to - from) * sizeof(Scene));
        else
            std::memmove(&g_bank.scenes[to + 1], &g_bank.scenes[to], (from - to) * sizeof(Scene));
        g_bank.scenes[to] = moved;
    }
    persist_scenes(remap_global_scene_refs(SceneEdit::Move, from, to));
    return true;
}

bool replace_scenes(const Scene* scenes, size_t count) {
    ScopedLock lock;
    if (count > kMaxScenes) count = kMaxScenes;
    {
        BankEdit edit;
        std::memset(&g_bank, 0, sizeof(g_bank));
        for (size_t i = 0; i < count; ++i) {
            g_bank.scenes[i] = scenes[i];
            sanitize_scene(g_bank.scenes[i]);
        }
        g_bank.count = static_cast<uint8_t>(count);
    }
    return persist_scenes(false);
}

void reset_to_defaults() {
    ScopedLock lock;
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
    nvs_save_blob(h, kKeyControl, &g_control, sizeof(g_control));
    nvs_save_blob(h, kKeyPlaylist, &g_playlist, sizeof(g_playlist));
    nvs_commit(h);
    nvs_close(h);
}

const ControlConfig& get_control() {
    return g_control;
}

bool set_control(const ControlConfig& cfg) {
    ScopedLock lock;
    g_control = cfg;
    sanitize_control(g_control);
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_save_blob(h, kKeyControl, &g_control, sizeof(g_control));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

const FseqPlaylist& get_playlist() {
    return g_playlist;
}

bool set_playlist(const FseqPlaylist& p) {
    ScopedLock lock;
    g_playlist = p;
    sanitize_playlist(g_playlist);
    if (!g_nvs_ok) return false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_save_blob(h, kKeyPlaylist, &g_playlist, sizeof(g_playlist));
    nvs_commit(h);
    nvs_close(h);
    return true;
}

}  // namespace pixfrog::config
