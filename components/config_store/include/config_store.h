// config_store — persistent configuration for pixfrog.
//
// Two flavors:
//   GlobalConfig   : IP/DHCP, ArtNet identity, refresh rate
//   ChannelConfig  : per-LED-channel settings (×8)
//
// All accessors are blocking on NVS. They are expected to be called only
// from `ui_task` and once during boot from `app_main`. The render path
// reads a RAM-cached snapshot instead — see `config_get_runtime_snapshot()`.

#pragma once

#include <array>
#include <stdint.h>

#include "led_protocols.h"

namespace pixfrog::config {

constexpr size_t kNumChannels = 8;

constexpr size_t kArtnetNameShortMax = 18;
constexpr size_t kArtnetNameLongMax  = 64;

// ────────────────────────────────────────────────────────────────────────────
// Network + ArtNet identity
// ────────────────────────────────────────────────────────────────────────────

struct GlobalConfig {
    // Network
    bool use_dhcp;
    uint32_t static_ip;  // host-order; 0 if use_dhcp
    uint32_t static_mask;
    uint32_t static_gateway;

    // ArtNet identity
    uint8_t artnet_net;     // 0..127
    uint8_t artnet_subnet;  // 0..15
    char short_name[kArtnetNameShortMax];
    char long_name[kArtnetNameLongMax];
    bool artnet_poll_reply_unicast;  // false=broadcast (default), true=unicast to poller

    // System
    uint8_t refresh_rate_hz;  // 30 or 60
    uint16_t home_timeout_s;  // 30 by default

    // Web UI — opt-in; no TCP socket opened when false (default)
    bool web_enabled;

    // sACN (E1.31) receiver — opt-in; no UDP socket opened when false (default)
    bool sacn_enabled;

    // Web UI admin password — salted SHA-256, never stored in clear.
    // hash all-zero = no password set = auth disabled (the default).
    uint8_t web_auth_salt[8];
    uint8_t web_auth_hash[32];  // SHA-256(salt || password)

    // Signal-loss failsafe — global setting, triggered per channel when no
    // packet touched any of its universes for failsafe_timeout_s. Zero-fill
    // migration = Hold + disabled = today's behaviour.
    uint8_t failsafe_mode;        // 0=hold last look, 1=blackout, 2=solid colour
    uint16_t failsafe_timeout_s;  // 0 = failsafe disabled
    uint8_t failsafe_r;           // solid-colour fill (mode 2)
    uint8_t failsafe_g;
    uint8_t failsafe_b;
};

constexpr uint8_t kFailsafeHold     = 0;
constexpr uint8_t kFailsafeBlackout = 1;
constexpr uint8_t kFailsafeColor    = 2;

// ────────────────────────────────────────────────────────────────────────────
// Per-channel configuration
// ────────────────────────────────────────────────────────────────────────────

struct ChannelConfig {
    led::Protocol protocol;
    led::ColorOrder color_order;
    uint16_t universe_start;  // 1..32767
    uint16_t dmx_start;       // 1..512
    uint16_t pixel_count;     // 1..1024
    uint8_t brightness;       // 0..255
    uint8_t grouping;         // 1..8
    bool invert_direction;
    uint32_t clock_hz;  // only for clocked protocols
};

// ────────────────────────────────────────────────────────────────────────────
// API
// ────────────────────────────────────────────────────────────────────────────

// Must be called once before any other API. Initializes NVS partition and
// loads defaults if the namespace is empty.
void init();

// Read-only access to current cached config (returns reference into static storage).
const GlobalConfig& get_global();
const ChannelConfig& get_channel(size_t channel_index);

// Mutating helpers — write to NVS and update the cache atomically.
// Return true on success.
bool set_global(const GlobalConfig& cfg);
bool set_channel(size_t channel_index, const ChannelConfig& cfg);

// ── Web UI admin password ───────────────────────────────────────────────────
// Empty/null password clears the hash (auth disabled). Setting a password
// generates a fresh random salt and stores SHA-256(salt || password).
// All three are ui_task/console-context only (NVS write path).
bool set_web_password(const char* password);
bool web_password_set();
bool check_web_password(const char* password);

// Restore defaults (factory reset). Does NOT reboot.
void reset_to_defaults();

// True if config_store successfully opened the NVS namespace at boot and
// is able to persist subsequent set_global/set_channel writes. False if
// NVS was corrupt beyond recovery and we're running on hard-coded RAM
// defaults — in that mode all setters update the cache but return false
// because nothing is written to flash.
bool is_persistence_ok();

}  // namespace pixfrog::config
