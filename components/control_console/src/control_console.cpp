#include "control_console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fseq_player.h"
#include "led_output.h"
#include "led_protocols.h"
#include "net.h"
#include "sacn.h"
#include "ui.h"
#include "web_config.h"

namespace pixfrog::console {

namespace {

constexpr const char* TAG = "CONSOLE";

const char* const kProtocolNames[] = { "Off",    "WS2815", "WS2812B", "WS2811", "SK6812",
                                       "WS2814", "APA102", "SK9822",  "LPD8806" };
static_assert(sizeof(kProtocolNames) / sizeof(kProtocolNames[0]) ==
              static_cast<size_t>(led::Protocol::COUNT));

const char* const kOrderNames[] = { "RGB", "RBG", "GRB", "GBR", "BRG", "BGR", "RGBW", "GRBW" };
static_assert(sizeof(kOrderNames) / sizeof(kOrderNames[0]) ==
              static_cast<size_t>(led::ColorOrder::COUNT));

// ── response helpers ────────────────────────────────────────────────────────
// Handlers always return 0 (via ok/err) so esp_console never appends its own
// "command returned non-zero" line and the OK/ERR terminator stays the last
// word of every response.

int ok() {
    printf("OK\n");
    return 0;
}

int err(const char* msg) {
    printf("ERR %s\n", msg);
    return 0;
}

// ── parse / format helpers ──────────────────────────────────────────────────

bool parse_u32(const char* s, uint32_t& out) {
    char* end             = nullptr;
    const unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_u32_in(const char* s, uint32_t lo, uint32_t hi, uint32_t& out) {
    return parse_u32(s, out) && out >= lo && out <= hi;
}

bool parse_bool(const char* s, bool& out) {
    uint32_t v = 0;
    if (!parse_u32(s, v) || v > 1) return false;
    out = (v != 0);
    return true;
}

bool parse_ip(const char* s, uint32_t& out) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

void fmt_ip(uint32_t ip, char* buf, size_t cap) {
    snprintf(buf, cap, "%u.%u.%u.%u", static_cast<unsigned>((ip >> 24) & 0xFFu),
             static_cast<unsigned>((ip >> 16) & 0xFFu), static_cast<unsigned>((ip >> 8) & 0xFFu),
             static_cast<unsigned>(ip & 0xFFu));
}

// Accepts the enum name (case-insensitive) or its numeric value.
int lookup_name(const char* const* names, size_t count, const char* s) {
    uint32_t v = 0;
    if (parse_u32(s, v)) return v < count ? static_cast<int>(v) : -1;
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(names[i], s) == 0) return static_cast<int>(i);
    }
    return -1;
}

void copy_str(char* dst, size_t cap, const char* src) {
    memset(dst, 0, cap);
    const size_t n = strlen(src);
    memcpy(dst, src, n < cap - 1 ? n : cap - 1);
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes `hex` into `out` (cap bytes). Returns decoded length, or 0 on
// odd-length/non-hex/overflow input.
size_t parse_hex(const char* hex, uint8_t* out, size_t cap) {
    const size_t hl = strlen(hex);
    if (hl == 0 || (hl % 2) != 0 || hl / 2 > cap) return 0;
    for (size_t i = 0; i < hl / 2; ++i) {
        const int hi = hex_nibble(hex[2 * i]);
        const int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return hl / 2;
}

void print_hex_line(const char* key, const uint8_t* data, size_t len) {
    printf("%s=", key);
    for (size_t i = 0; i < len; ++i)
        printf("%02x", data[i]);
    printf("\n");
}

// ── version / status / stats ────────────────────────────────────────────────

int cmd_version(int, char**) {
    const esp_app_desc_t* app = esp_app_get_description();
    printf("project=%s\n", app->project_name);
    printf("version=%s\n", app->version);
    printf("idf=%s\n", esp_get_idf_version());
    printf("compile=%s %s\n", app->date, app->time);
    printf("partition=%s\n", esp_ota_get_running_partition()->label);
    config::RollbackRecord rb{};
    if (config::get_rollback(rb)) {
        printf("last_rollback=%s on %s rejected, running %s, reset_reason=%u, acknowledged=%u\n",
               rb.rejected_version, rb.rejected_slot, rb.running_version, rb.reset_reason,
               rb.acknowledged);
    } else {
        printf("last_rollback=none\n");
    }
    return ok();
}

// rollback ack — mark the recorded OTA rollback as seen (clears the web banner).
int cmd_rollback(int argc, char** argv) {
    if (argc != 2 || strcmp(argv[1], "ack") != 0) return err("usage: rollback ack");
    config::RollbackRecord rb{};
    if (!config::get_rollback(rb)) return err("no rollback recorded");
    rb.acknowledged = 1;
    if (!config::set_rollback(rb)) printf("warn=not_persisted\n");
    return ok();
}

int cmd_status(int, char**) {
    char ip[16];
    fmt_ip(ui::get_ip(), ip, sizeof(ip));
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_ETH);

    printf("uptime_ms=%lld\n", esp_timer_get_time() / 1000);
    printf("link=%d\n", ui::is_link_up() ? 1 : 0);
    printf("ip=%s\n", ip);
    printf("mac=%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("fps=%lu\n", static_cast<unsigned long>(dmx::get_stats().current_fps));
    printf("cal=%d\n", static_cast<int>(output::get_calibration_mode()));
    printf("persist_ok=%d\n", config::is_persistence_ok() ? 1 : 0);
    printf("fb_bytes=%u\n", static_cast<unsigned>(output::fb_bytes()));
    printf("heap_free=%u\n", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    printf("psram_free=%u\n", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ok();
}

int cmd_stats(int, char**) {
    const dmx::Stats s = dmx::get_stats();
    printf("frames_emitted=%llu\n", static_cast<unsigned long long>(s.frames_emitted));
    printf("artnet_packets_rx=%llu\n", static_cast<unsigned long long>(s.artnet_packets_rx));
    printf("artnet_bad_packets=%llu\n", static_cast<unsigned long long>(s.artnet_bad_packets));
    printf("artnet_ctrl_rx=%llu\n", static_cast<unsigned long long>(s.artnet_ctrl_rx));
    printf("sacn_packets_rx=%llu\n", static_cast<unsigned long long>(s.sacn_packets_rx));
    printf("dma_underruns=%lu\n", static_cast<unsigned long>(s.dma_underruns));
    printf("current_fps=%lu\n", static_cast<unsigned long>(s.current_fps));
    const output::DebugCounters d = output::get_debug_counters();
    printf("lcd_trans_done=%lu\n", static_cast<unsigned long>(d.trans_done));
    printf("lcd_vsync=%lu\n", static_cast<unsigned long>(d.vsync));
    printf("lcd_msync_err=%lu\n", static_cast<unsigned long>(d.msync_err));
    printf("render_wait_us=%lu\n", static_cast<unsigned long>(d.wait_us));
    printf("render_encode_us=%lu\n", static_cast<unsigned long>(d.encode_us));
    printf("render_submit_us=%lu\n", static_cast<unsigned long>(d.submit_us));
    printf("render_decode_us=%lu\n", static_cast<unsigned long>(s.decode_us));
    printf("render_decode_max_us=%lu\n", static_cast<unsigned long>(s.decode_max_us));
    for (int core = 0; core < 2; ++core)
        if (s.cpu_load[core] == dmx::kCpuLoadUnknown)
            printf("cpu%d_load=-\n", core);
        else
            printf("cpu%d_load=%u\n", core, static_cast<unsigned>(s.cpu_load[core]));
    return ok();
}

// tasks — each task's share of a core over half a second, like top:
// "task=render core=1 prio=20 cpu=46" (core -1: either core).
int cmd_tasks(int argc, char**) {
    if (argc != 1) return err("usage: tasks");
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
    constexpr UBaseType_t kMax = 40;
    static TaskStatus_t before[kMax], after[kMax];  // off the console task's stack
    configRUN_TIME_COUNTER_TYPE t0 = 0, t1 = 0;
    const UBaseType_t n0 = uxTaskGetSystemState(before, kMax, &t0);
    vTaskDelay(pdMS_TO_TICKS(500));
    const UBaseType_t n1 = uxTaskGetSystemState(after, kMax, &t1);
    const auto window    = static_cast<uint32_t>(t1 - t0);
    if (window == 0) return err("no run time measured");
    for (UBaseType_t i = 0; i < n1; ++i) {
        uint32_t ran = 0;
        for (UBaseType_t j = 0; j < n0; ++j)
            if (before[j].xHandle == after[i].xHandle)
                ran = static_cast<uint32_t>(after[i].ulRunTimeCounter - before[j].ulRunTimeCounter);
#if configTASKLIST_INCLUDE_COREID
        const int core = after[i].xCoreID == tskNO_AFFINITY ? -1
                                                            : static_cast<int>(after[i].xCoreID);
#else
        const int core = -1;
#endif
        printf("task=%s core=%d prio=%u cpu=%lu\n", after[i].pcTaskName, core,
               static_cast<unsigned>(after[i].uxCurrentPriority),
               static_cast<unsigned long>(static_cast<uint64_t>(ran) * 100u / window));
    }
    return ok();
#else
    return err("run time stats not built in");
#endif
}

int cmd_chstat(int, char**) {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& c = config::get_channel(ch);
        printf("ch%u protocol=%s universe=%u pixels=%u active=%d capacity_ok=%d failsafe=%d "
               "merge=%d\n",
               static_cast<unsigned>(ch), kProtocolNames[static_cast<size_t>(c.protocol)],
               c.universe_start, c.pixel_count, dmx::is_channel_active(ch) ? 1 : 0,
               dmx::is_channel_capacity_ok(ch) ? 1 : 0, dmx::is_channel_failsafe(ch) ? 1 : 0,
               dmx::is_channel_merging(ch) ? 1 : 0);
    }
    return ok();
}

// ── global config ───────────────────────────────────────────────────────────

void print_global(const config::GlobalConfig& g) {
    char ip[16], mask[16], gw[16];
    fmt_ip(g.static_ip, ip, sizeof(ip));
    fmt_ip(g.static_mask, mask, sizeof(mask));
    fmt_ip(g.static_gateway, gw, sizeof(gw));
    printf("dhcp=%d\n", g.use_dhcp ? 1 : 0);
    printf("ip_fallback=%s\n", config::ip_fallback_id(g.ip_fallback));
    printf("ip=%s\n", ip);
    printf("mask=%s\n", mask);
    printf("gw=%s\n", gw);
    printf("net=%u\n", g.artnet_net);
    printf("subnet=%u\n", g.artnet_subnet);
    printf("short_name=%s\n", g.short_name);
    printf("long_name=%s\n", g.long_name);
    printf("reply_unicast=%d\n", g.artnet_poll_reply_unicast ? 1 : 0);
    printf("refresh_hz=%u\n", g.refresh_rate_hz);
    printf("home_timeout_s=%u\n", g.home_timeout_s);
    printf("tft_brightness=%u\n", config::tft_brightness_pct(g));
    printf("tft_idle_dim=%u\n", config::tft_idle_dim_pct(g));
    printf("tft_dim_delay_s=%u\n", config::tft_dim_delay_s(g));
    printf("web_enabled=%d\n", g.web_enabled ? 1 : 0);
    printf("sacn_enabled=%d\n", g.sacn_enabled ? 1 : 0);
    printf("fpp_remote=%d\n", g.fpp_remote ? 1 : 0);
    printf("web_auth=%d\n", config::web_password_set() ? 1 : 0);
    static const char* const kFailsafeNames[] = { "hold", "blackout", "color", "scene" };
    printf("failsafe_mode=%s\n", kFailsafeNames[g.failsafe_mode <= 3 ? g.failsafe_mode : 0]);
    printf("failsafe_timeout_s=%u\n", g.failsafe_timeout_s);
    printf("failsafe_color=%02x%02x%02x\n", g.failsafe_r, g.failsafe_g, g.failsafe_b);
    printf("failsafe_scene=%u\n", g.failsafe_scene);
    printf("boot_scene=%u\n", g.boot_scene);
    printf("merge_mode=%s\n", g.merge_mode == config::kMergeLtp ? "LTP" : "HTP");
    printf("language=%s\n", g.language == config::kLangFrench ? "fr" : "en");
    printf("hub_preferred=%d\n", g.hub_preferred ? 1 : 0);
    printf("fseq_universe=%u\n", config::fseq_universe(g));
    printf("speaker_volume=%u\n", config::speaker_volume_pct(g));
}

int cmd_global(int argc, char** argv) {
    if (argc == 1) {
        print_global(config::get_global());
        return ok();
    }
    if (argc != 3) return err("usage: global [<key> <value>]");

    config::GlobalConfig g = config::get_global();
    const char* key        = argv[1];
    const char* val        = argv[2];
    bool network_changed   = false;
    uint32_t u             = 0;

    if (strcmp(key, "dhcp") == 0) {
        if (!parse_bool(val, g.use_dhcp)) return err("dhcp: 0|1");
        network_changed = true;
    } else if (strcmp(key, "language") == 0) {  // web UI language
        if (strcmp(val, "en") == 0 || strcmp(val, "0") == 0)
            g.language = config::kLangEnglish;
        else if (strcmp(val, "fr") == 0 || strcmp(val, "1") == 0)
            g.language = config::kLangFrench;
        else
            return err("language: en|fr");
    } else if (strcmp(key, "hub_preferred") == 0) {  // answers pixfrog.local first
        bool b = false;
        if (!parse_bool(val, b)) return err("hub_preferred: 0|1");
        g.hub_preferred = b ? 1 : 0;
    } else if (strcmp(key, "fseq_universe") == 0) {  // universe of FSEQ byte 0
        if (!parse_u32_in(val, 1, dmx::kMaxUniverseNumber, u))
            return err("fseq_universe: 1..32767");
        g.fseq_universe = static_cast<uint16_t>(u);
    } else if (strcmp(key, "speaker_volume") == 0) {  // 0 = off
        if (!parse_u32_in(val, 0, 100, u)) return err("speaker_volume: 0..100 (0=off)");
        g.speaker_volume = static_cast<uint8_t>(u);
    } else if (strcmp(key, "ip_fallback") == 0) {
        const int fb = config::ip_fallback_from_id(val);
        if (fb < 0) return err("ip_fallback: linklocal|artnet");
        g.ip_fallback = static_cast<uint8_t>(fb);
    } else if (strcmp(key, "ip") == 0) {
        if (!parse_ip(val, g.static_ip)) return err("ip: a.b.c.d");
        network_changed = true;
    } else if (strcmp(key, "mask") == 0) {
        if (!parse_ip(val, g.static_mask)) return err("mask: a.b.c.d");
        network_changed = true;
    } else if (strcmp(key, "gw") == 0) {
        if (!parse_ip(val, g.static_gateway)) return err("gw: a.b.c.d");
        network_changed = true;
    } else if (strcmp(key, "net") == 0) {
        if (!parse_u32_in(val, 0, 127, u)) return err("net: 0..127");
        g.artnet_net = static_cast<uint8_t>(u);
    } else if (strcmp(key, "subnet") == 0) {
        if (!parse_u32_in(val, 0, 15, u)) return err("subnet: 0..15");
        g.artnet_subnet = static_cast<uint8_t>(u);
    } else if (strcmp(key, "short_name") == 0) {
        copy_str(g.short_name, sizeof(g.short_name), val);
    } else if (strcmp(key, "long_name") == 0) {
        copy_str(g.long_name, sizeof(g.long_name), val);
    } else if (strcmp(key, "reply_unicast") == 0) {
        if (!parse_bool(val, g.artnet_poll_reply_unicast)) return err("reply_unicast: 0|1");
    } else if (strcmp(key, "refresh_hz") == 0) {
        if (!parse_u32_in(val, config::kMinRefreshHz, config::kMaxRefreshHz, u))
            return err("refresh_hz: 20..120");
        g.refresh_rate_hz = static_cast<uint8_t>(u);
    } else if (strcmp(key, "home_timeout_s") == 0) {
        if (!parse_u32_in(val, 0, 65535, u)) return err("home_timeout_s: 0..65535");
        g.home_timeout_s = static_cast<uint16_t>(u);
    } else if (strcmp(key, "tft_brightness") == 0) {
        if (!parse_u32_in(val, config::kTftBrightnessMin, 100, u))
            return err("tft_brightness: 10..100 (%)");
        g.tft_brightness = static_cast<uint8_t>(u);
    } else if (strcmp(key, "tft_idle_dim") == 0) {
        if (!parse_u32_in(val, 0, 100, u)) return err("tft_idle_dim: 0..100 (0=never dim)");
        g.tft_idle_dim = static_cast<uint8_t>(u);
    } else if (strcmp(key, "tft_dim_delay_s") == 0) {
        if (!parse_u32_in(val, 0, config::kTftDimDelayMaxS, u))
            return err("tft_dim_delay_s: 0..3600 (0=never dim)");
        g.tft_dim_delay_s = static_cast<uint16_t>(u);
    } else if (strcmp(key, "web_enabled") == 0) {
        if (!parse_bool(val, g.web_enabled)) return err("web_enabled: 0|1");
    } else if (strcmp(key, "sacn_enabled") == 0) {
        if (!parse_bool(val, g.sacn_enabled)) return err("sacn_enabled: 0|1");
    } else if (strcmp(key, "fpp_remote") == 0) {
        if (!parse_bool(val, g.fpp_remote)) return err("fpp_remote: 0|1");
    } else if (strcmp(key, "failsafe_mode") == 0) {
        static const char* const kFailsafeNames[] = { "hold", "blackout", "color", "scene" };
        const int m                               = lookup_name(kFailsafeNames, 4, val);
        if (m < 0) return err("failsafe_mode: hold|blackout|color|scene or 0..3");
        g.failsafe_mode = static_cast<uint8_t>(m);
    } else if (strcmp(key, "failsafe_scene") == 0) {
        if (config::num_scenes() == 0 || !parse_u32_in(val, 0, config::num_scenes() - 1, u))
            return err("failsafe_scene: an existing scene index (see `scene`)");
        g.failsafe_scene = static_cast<uint8_t>(u);
    } else if (strcmp(key, "boot_scene") == 0) {
        if (!parse_u32_in(val, 0, config::num_scenes(), u))
            return err("boot_scene: 0=none, N = scene N-1");
        g.boot_scene = static_cast<uint8_t>(u);
    } else if (strcmp(key, "failsafe_timeout_s") == 0) {
        if (!parse_u32_in(val, 0, 3600, u)) return err("failsafe_timeout_s: 0..3600 (0=off)");
        g.failsafe_timeout_s = static_cast<uint16_t>(u);
    } else if (strcmp(key, "failsafe_color") == 0) {
        uint8_t rgb[3];
        if (parse_hex(val, rgb, 3) != 3) return err("failsafe_color: rrggbb");
        g.failsafe_r = rgb[0];
        g.failsafe_g = rgb[1];
        g.failsafe_b = rgb[2];
    } else if (strcmp(key, "merge_mode") == 0) {
        static const char* const kMergeNames[] = { "HTP", "LTP" };
        const int m                            = lookup_name(kMergeNames, 2, val);
        if (m < 0) return err("merge_mode: HTP|LTP or 0..1");
        g.merge_mode = static_cast<uint8_t>(m);
    } else if (strcmp(key, "web_password") == 0) {
        // UART = the trusted physical recovery channel. `-` clears (auth off).
        const bool cleared = (strcmp(val, "-") == 0);
        if (!cleared && strlen(val) > config::kMaxWebPasswordLen)
            return err("web_password: at most 63 characters");
        if (!config::set_web_password(cleared ? "" : val)) printf("warn=not_persisted\n");
        printf("web_auth=%d\n", config::web_password_set() ? 1 : 0);
        return ok();
    } else {
        return err("unknown key (dhcp ip_fallback language hub_preferred fseq_universe "
                   "speaker_volume ip mask gw "
                   "net subnet "
                   "short_name long_name "
                   "reply_unicast "
                   "refresh_hz home_timeout_s tft_brightness tft_idle_dim tft_dim_delay_s "
                   "web_enabled "
                   "sacn_enabled fpp_remote web_password "
                   "failsafe_mode failsafe_timeout_s failsafe_color failsafe_scene boot_scene "
                   "merge_mode)");
    }

    const bool persisted = config::set_global(g);
    dmx::mark_global_dirty();
    if (!persisted) printf("warn=not_persisted\n");
    if (network_changed) {  // live: the box re-addresses itself now
        net::apply(g);
        printf("note=network_applied\n");
    }

    // Apply server states immediately (no reboot needed).
    if (strcmp(key, "web_enabled") == 0) {
        if (g.web_enabled)
            web::start();
        else
            web::stop();
    }
    if (strcmp(key, "sacn_enabled") == 0) {
        if (g.sacn_enabled)
            sacn::start();
        else
            sacn::stop();
    }
    if (strcmp(key, "fpp_remote") == 0) {
        if (g.fpp_remote)
            fpp::start();
        else
            fpp::stop();
    }

    return ok();
}

// ── per-channel config ──────────────────────────────────────────────────────

void print_channel(size_t ch, const config::ChannelConfig& c) {
    printf("channel=%u\n", static_cast<unsigned>(ch));
    printf("protocol=%s\n", kProtocolNames[static_cast<size_t>(c.protocol)]);
    printf("order=%s\n", kOrderNames[static_cast<size_t>(c.color_order)]);
    printf("universe=%u\n", c.universe_start);
    printf("dmx_start=%u\n", c.dmx_start);
    printf("pixels=%u\n", c.pixel_count);
    printf("brightness=%u\n", c.brightness);
    printf("grouping=%u\n", c.grouping);
    printf("invert=%d\n", c.invert_direction ? 1 : 0);
    printf("clock_hz=%lu\n", static_cast<unsigned long>(c.clock_hz));
    printf("gamma_x10=%u\n", c.gamma_x10);
    printf("wb=%02x%02x%02x\n", c.wb_r, c.wb_g, c.wb_b);
    // first dead LED (1-based):count, comma-separated; "-" = none
    const size_t ng = led::gap_count(c.gaps, led::kMaxPixelGaps);
    printf("gaps=");
    for (size_t k = 0; k < ng; ++k)
        printf("%s%u:%u", k ? "," : "", c.gaps[k].pos + 1u, static_cast<unsigned>(c.gaps[k].len));
    printf("%s\n", ng ? "" : "-");
    // What drives it from the network: pixel mapping (packing = its layout, from
    // universe / dmx_start) and fixture control (from fix_universe / fix_dmx).
    printf("pixel_map=%d\n", config::pixel_mapped(c) ? 1 : 0);
    printf("packing=%s\n", config::packing_id(config::pixel_layout(c)));
    printf("fixture_ctl=%d\n", config::fixture_controlled(c) ? 1 : 0);
    printf("fix_universe=%u\n", config::fix_universe(c));
    printf("fix_dmx=%u\n", config::fix_dmx_start(c));
    printf("universes=%u\n", static_cast<unsigned>(dmx::channel_universe_span(c)));
    // first LED (1-based):count[:r][:pN], comma-separated; "-" = none
    const size_t nf = config::fixture_count(c.fixtures, config::kMaxFixtures);
    printf("fixtures=");
    for (size_t k = 0; k < nf; ++k) {
        printf("%s%u:%u", k ? "," : "", c.fixtures[k].pos + 1u, config::fixture_len(c.fixtures[k]));
        if (config::fixture_reversed(c.fixtures[k])) printf(":r");
        if (config::fixture_profile(c.fixtures[k]))
            printf(":p%u", config::fixture_profile(c.fixtures[k]));
    }
    printf("%s\n", nf ? "" : "-");
    // Fixture control — the patch sheet: universe.address+channels per fixture
    if (config::fixture_controlled(c)) {
        static dmx::FixtureAddress at[config::kMaxFixtures];  // off the console task's stack
        const size_t n = dmx::fixture_patch(c, at, config::kMaxFixtures);
        printf("patch=");
        for (size_t k = 0; k < n; ++k)
            printf("%s%u.%u+%u", k ? "," : "", at[k].universe, at[k].address, at[k].footprint);
        printf("\n");
    }
}

// "pos:len[,pos:len…]" (pos 1-based) or "-" → gaps; false on a malformed list.
bool parse_gaps(const char* arg, led::PixelGap out[led::kMaxPixelGaps]) {
    for (size_t k = 0; k < led::kMaxPixelGaps; ++k)
        out[k] = led::PixelGap{ 0, 0 };
    if (strcmp(arg, "-") == 0) return true;
    char buf[led::kMaxPixelGaps * 10 + 1];
    if (strlen(arg) >= sizeof(buf)) return false;
    copy_str(buf, sizeof(buf), arg);
    size_t n   = 0;
    char* save = nullptr;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        char* colon = strchr(tok, ':');
        if (!colon || n == led::kMaxPixelGaps) return false;
        *colon       = '\0';
        uint32_t pos = 0;
        uint32_t len = 0;
        if (!parse_u32_in(tok, 1, led::kMaxPixelsPerChannel, pos) ||
            !parse_u32_in(colon + 1, 1, led::kMaxPixelsPerChannel, len))
            return false;
        out[n++] = led::PixelGap{ static_cast<uint16_t>(pos - 1), static_cast<uint16_t>(len) };
    }
    return n > 0;
}

// "first:count[:r][:pN],…" (first 1-based) or "-" for none → the fixture
// list. False on a malformed entry, a fixture past the strip's limits or two
// fixtures sharing an LED.
bool parse_fixtures(const char* arg, config::Fixture out[config::kMaxFixtures]) {
    config::Fixture parsed[config::kMaxFixtures] = {};
    size_t n                                     = 0;
    if (strcmp(arg, "-") != 0) {
        static char buf[config::kMaxFixtures * 16];  // off the console task's stack
        if (strlen(arg) >= sizeof(buf)) return false;
        copy_str(buf, sizeof(buf), arg);
        char* save = nullptr;
        for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
            if (n == config::kMaxFixtures) return false;
            uint32_t first = 0, count = 0, profile = 0;
            bool reversed = false;
            char* save2   = nullptr;
            int field     = 0;
            for (char* f = strtok_r(tok, ":", &save2); f;
                 f       = strtok_r(nullptr, ":", &save2), ++field) {
                if (field == 0) {
                    if (!parse_u32_in(f, 1, led::kMaxPixelsPerChannel, first)) return false;
                } else if (field == 1) {
                    if (!parse_u32_in(f, 1, led::kMaxPixelsPerChannel, count)) return false;
                } else if (strcmp(f, "r") == 0) {
                    reversed = true;
                } else if (f[0] != 'p' ||
                           !parse_u32_in(f + 1, 0, config::kMaxProfiles - 1, profile)) {
                    return false;
                }
            }
            if (field < 2 || first + count - 1 > led::kMaxPixelsPerChannel) return false;
            parsed[n++] = config::make_fixture(static_cast<uint16_t>(first - 1),
                                               static_cast<uint16_t>(count), reversed,
                                               static_cast<uint8_t>(profile));
        }
        for (size_t a = 0; a < n; ++a)
            for (size_t b = a + 1; b < n; ++b)
                if (parsed[a].pos < parsed[b].pos + config::fixture_len(parsed[b]) &&
                    parsed[b].pos < parsed[a].pos + config::fixture_len(parsed[a]))
                    return false;
    }
    memcpy(out, parsed, sizeof(parsed));
    return true;
}

int cmd_ch(int argc, char** argv) {
    if (argc != 2 && argc != 4) return err("usage: ch <n> [<key> <value>]");
    uint32_t ch = 0;
    if (!parse_u32_in(argv[1], 0, config::kNumChannels - 1, ch)) return err("channel: 0..7");

    if (argc == 2) {
        print_channel(ch, config::get_channel(ch));
        return ok();
    }

    config::ChannelConfig c = config::get_channel(ch);
    const char* key         = argv[2];
    const char* val         = argv[3];
    uint32_t u              = 0;

    if (strcmp(key, "protocol") == 0) {
        const int p = lookup_name(kProtocolNames, static_cast<size_t>(led::Protocol::COUNT), val);
        if (p < 0)
            return err("protocol: Off|WS2815|WS2812B|WS2811|SK6812|WS2814|APA102|SK9822|"
                       "LPD8806 or 0..8");
        c.protocol = static_cast<led::Protocol>(p);
    } else if (strcmp(key, "order") == 0) {
        const int o = lookup_name(kOrderNames, static_cast<size_t>(led::ColorOrder::COUNT), val);
        if (o < 0) return err("order: RGB|RBG|GRB|GBR|BRG|BGR|RGBW|GRBW or 0..7");
        c.color_order = static_cast<led::ColorOrder>(o);
    } else if (strcmp(key, "universe") == 0) {
        if (!parse_u32_in(val, 0, 32767, u)) return err("universe: 0..32767");
        c.universe_start = static_cast<uint16_t>(u);
    } else if (strcmp(key, "dmx_start") == 0) {
        if (!parse_u32_in(val, 1, 512, u)) return err("dmx_start: 1..512");
        c.dmx_start = static_cast<uint16_t>(u);
    } else if (strcmp(key, "pixels") == 0) {
        if (!parse_u32_in(val, 1, dmx::kMaxPixelsPerChan, u)) return err("pixels: 1..1024");
        c.pixel_count = static_cast<uint16_t>(u);
    } else if (strcmp(key, "brightness") == 0) {
        if (!parse_u32_in(val, 0, 255, u)) return err("brightness: 0..255");
        c.brightness = static_cast<uint8_t>(u);
    } else if (strcmp(key, "grouping") == 0) {
        if (!parse_u32_in(val, 1, 8, u)) return err("grouping: 1..8");
        c.grouping = static_cast<uint8_t>(u);
    } else if (strcmp(key, "invert") == 0) {
        if (!parse_bool(val, c.invert_direction)) return err("invert: 0|1");
    } else if (strcmp(key, "gamma_x10") == 0) {
        if (!parse_u32_in(val, 10, 40, u)) return err("gamma_x10: 10 (linear)..40");
        c.gamma_x10 = static_cast<uint8_t>(u);
    } else if (strcmp(key, "wb") == 0) {
        uint8_t rgb[3];
        if (parse_hex(val, rgb, 3) != 3) return err("wb: rrggbb (ffffff = unity)");
        c.wb_r = rgb[0] ? rgb[0] : 255;
        c.wb_g = rgb[1] ? rgb[1] : 255;
        c.wb_b = rgb[2] ? rgb[2] : 255;
    } else if (strcmp(key, "clock_hz") == 0) {
        if (!parse_u32_in(val, led::kMinClockHz, led::kMaxClockHz, u))
            return err("clock_hz: 500000..8000000");
        c.clock_hz = u;
    } else if (strcmp(key, "gaps") == 0) {
        if (!parse_gaps(val, c.gaps))
            return err("gaps: pos:len[,pos:len...] (pos 1-based, max 8) or -");
    } else if (strcmp(key, "packing") == 0) {
        const int p = config::packing_from_id(val);
        if (p < 0) return err("packing: continuous|whole|fixture|colour");
        if (p == config::kPackControl) {  // as before the two switches: fixtures alone
            config::set_fix_address(c, c.universe_start, c.dmx_start);
            config::set_dmx_modes(c, false, true);
        } else {
            config::set_pixel_layout(c, static_cast<uint8_t>(p));
        }
    } else if (strcmp(key, "pixel_map") == 0 || strcmp(key, "fixture_ctl") == 0) {
        if (!parse_u32_in(val, 0, 1, u)) return err("pixel_map, fixture_ctl: 0|1");
        if (key[0] == 'p')
            config::set_dmx_modes(c, u != 0, config::fixture_controlled(c));
        else
            config::set_dmx_modes(c, config::pixel_mapped(c), u != 0);
    } else if (strcmp(key, "fix_universe") == 0) {
        if (!parse_u32_in(val, 0, 32767, u)) return err("fix_universe: 0..32767");
        config::set_fix_address(c, static_cast<uint16_t>(u), config::fix_dmx_start(c));
    } else if (strcmp(key, "fix_dmx") == 0) {
        if (!parse_u32_in(val, 1, 512, u)) return err("fix_dmx: 1..512");
        config::set_fix_address(c, config::fix_universe(c), static_cast<uint16_t>(u));
    } else if (strcmp(key, "fixtures") == 0) {
        if (!parse_fixtures(val, c.fixtures))
            return err("fixtures: first:count[:r][:pN],... (first 1-based, r = reversed, "
                       "pN = DMX profile N; max 32, no overlap) or -");
    } else {
        return err("unknown key (protocol order universe dmx_start pixels brightness grouping "
                   "invert clock_hz gamma_x10 wb gaps packing fixtures pixel_map fixture_ctl "
                   "fix_universe fix_dmx)");
    }

    const bool persisted = config::set_channel(ch, c);
    dmx::mark_channel_dirty(ch);
    if (!persisted) printf("warn=not_persisted\n");
    return ok();
}

// autopatch <base> [compact] [continuous|whole|fixture|colour] [fix <universe>]
int cmd_autopatch(int argc, char** argv) {
    const char* usage = "usage: autopatch <base_universe> [compact] "
                        "[continuous|whole|fixture|colour] [fix <universe>]";
    if (argc < 2 || argc > 6) return err(usage);
    uint32_t base = 0;
    if (!parse_u32_in(argv[1], 0, 32767, base)) return err("base_universe: 0..32767");
    dmx::AutoPatch o;
    o.base = static_cast<uint16_t>(base);
    for (int i = 2; i < argc; ++i) {
        const int p = config::packing_from_id(argv[i]);
        uint32_t fb = 0;
        if (strcmp(argv[i], "compact") == 0)
            o.compact = true;
        else if (strcmp(argv[i], "fix") == 0 && i + 1 < argc &&
                 parse_u32_in(argv[i + 1], 0, 32767, fb))
            o.fix_base = static_cast<int32_t>(fb), ++i;  // where the fixtures' block starts
        else if (p >= 0 && p != config::kPackControl)  // a mode of an output, not a layout for all
            o.packing = static_cast<int8_t>(p);
        else
            return err(usage);
    }

    uint16_t next        = 0;
    size_t universes     = 0;
    const bool persisted = dmx::auto_patch(o, &next, &universes);
    for (size_t i = 0; i < config::kNumChannels; ++i) {
        const auto& c = config::get_channel(i);
        printf("ch%u=universe %u dmx %u %s", static_cast<unsigned>(i),
               static_cast<unsigned>(c.universe_start), static_cast<unsigned>(c.dmx_start),
               config::pixel_mapped(c) ? config::packing_id(config::pixel_layout(c)) : "-");
        if (config::fixture_controlled(c))
            printf(" fixtures %u.%u", config::fix_universe(c), config::fix_dmx_start(c));
        printf("\n");
    }
    printf("next_free=%u\n", static_cast<unsigned>(next));
    printf("universes=%u/%u\n", static_cast<unsigned>(universes),
           static_cast<unsigned>(dmx::kNumUniverses));
    if (universes > dmx::kNumUniverses) printf("warn=pool_full\n");
    if (!persisted) printf("warn=not_persisted\n");
    return ok();
}

// overlaps — the ranges of the patch that share DMX channels, as the box
// computes them: "overlap=ch0 pixels,ch1 fixtures,U3.1+170" per pair (the
// first shared channel and how many they share).
void print_patch_owner(uint8_t owner) {
    if (owner >= dmx::kPatchControl)
        printf("control");
    else
        printf("ch%u %s", static_cast<unsigned>(owner % config::kNumChannels),
               owner < config::kNumChannels ? "pixels" : "fixtures");
}

int cmd_overlaps(int argc, char**) {
    if (argc != 1) return err("usage: overlaps");
    static dmx::PatchClash clashes[dmx::kMaxPatchClashes];  // off the console task's stack
    const size_t n = dmx::patch_clashes(clashes, dmx::kMaxPatchClashes);
    printf("overlaps=%u\n", static_cast<unsigned>(n));
    for (size_t i = 0; i < n; ++i) {
        printf("overlap=");
        print_patch_owner(clashes[i].a);
        printf(",");
        print_patch_owner(clashes[i].b);
        printf(",U%u.%u+%u\n", clashes[i].universe, clashes[i].address, clashes[i].channels);
    }
    return ok();
}

// ── DMX injection & buffer readback ─────────────────────────────────────────

int cmd_dmxw(int argc, char** argv) {
    if (argc != 4) return err("usage: dmxw <universe> <start_slot> <hexbytes>");
    uint32_t uni = 0, start = 0;
    if (!parse_u32_in(argv[1], 0, 32767, uni)) return err("universe: 0..32767");
    if (!parse_u32_in(argv[2], 1, dmx::kUniverseSize, start)) return err("start_slot: 1..512");

    uint8_t buf[dmx::kUniverseSize];
    const size_t len = parse_hex(argv[3], buf, sizeof(buf));
    if (len == 0) return err("hexbytes: even-length hex string, max 1024 chars");
    if (start - 1 + len > dmx::kUniverseSize) return err("write overflows universe");

    if (!dmx::inject_universe(static_cast<uint16_t>(uni), start - 1, buf, len)) {
        return err("universe not mapped to any channel (check ch <n> universe)");
    }
    printf("written=%u\n", static_cast<unsigned>(len));
    return ok();
}

int cmd_dmxr(int argc, char** argv) {
    if (argc != 2 && argc != 4) return err("usage: dmxr <universe> [<start_slot> <len>]");
    uint32_t uni = 0, start = 1, len = dmx::kUniverseSize;
    if (!parse_u32_in(argv[1], 0, 32767, uni)) return err("universe: 0..32767");
    if (argc == 4) {
        if (!parse_u32_in(argv[2], 1, dmx::kUniverseSize, start)) return err("start_slot: 1..512");
        if (!parse_u32_in(argv[3], 1, dmx::kUniverseSize, len)) return err("len: 1..512");
    }
    if (start - 1 + len > dmx::kUniverseSize) return err("read overflows universe");

    const uint8_t* front = dmx::universe_front_buffer_for(static_cast<uint16_t>(uni));
    if (!front) return err("universe not mapped to any channel");
    print_hex_line("data", front + start - 1, len);
    return ok();
}

int cmd_pixr(int argc, char** argv) {
    if (argc != 2 && argc != 4) return err("usage: pixr <ch> [<start_byte> <len>]");
    uint32_t ch = 0;
    if (!parse_u32_in(argv[1], 0, config::kNumChannels - 1, ch)) return err("channel: 0..7");

    const auto& c     = config::get_channel(ch);
    const size_t used = c.pixel_count * led::bytes_per_pixel(c.protocol);
    uint32_t start = 0, len = used;
    if (argc == 4) {
        if (!parse_u32(argv[2], start)) return err("start_byte: number");
        if (!parse_u32(argv[3], len)) return err("len: number");
    }
    if (used == 0) return err("channel is Off");
    if (start + len > used) return err("read past decoded region");

    const uint8_t* front = dmx::pixel_front_buffer(ch);
    if (!front) return err("pixel buffer unavailable");
    printf("bytes_used=%u\n", static_cast<unsigned>(used));
    print_hex_line("data", front + start, len);
    return ok();
}

// ── calibration / logs / lifecycle ──────────────────────────────────────────

int cmd_cal(int argc, char** argv) {
    if (argc == 1) {
        printf("cal=%d\n", static_cast<int>(output::get_calibration_mode()));
        return ok();
    }
    if (argc != 2) return err("usage: cal [-1|0|1|2|3]");
    char* end    = nullptr;
    const long v = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || v < -1 || v > 3) return err("mode: -1..3");
    output::set_calibration_mode(static_cast<int8_t>(v));
    printf("cal=%d\n", static_cast<int>(v));
    return ok();
}

int cmd_loglevel(int argc, char** argv) {
    if (argc != 2) return err("usage: loglevel <none|error|warn|info|debug|verbose>");
    esp_log_level_t lvl;
    if (strcmp(argv[1], "none") == 0)
        lvl = ESP_LOG_NONE;
    else if (strcmp(argv[1], "error") == 0)
        lvl = ESP_LOG_ERROR;
    else if (strcmp(argv[1], "warn") == 0)
        lvl = ESP_LOG_WARN;
    else if (strcmp(argv[1], "info") == 0)
        lvl = ESP_LOG_INFO;
    else if (strcmp(argv[1], "debug") == 0)
        lvl = ESP_LOG_DEBUG;
    else if (strcmp(argv[1], "verbose") == 0)
        lvl = ESP_LOG_VERBOSE;
    else
        return err("level: none|error|warn|info|debug|verbose");
    esp_log_level_set("*", lvl);
    printf("loglevel=%s\n", argv[1]);
    return ok();
}

int cmd_factory_reset(int, char**) {
    config::reset_to_defaults();
    dmx::mark_global_dirty();
    for (size_t ch = 0; ch < config::kNumChannels; ++ch)
        dmx::mark_channel_dirty(ch);
    // The defaults turn the opt-in services off and the box back on DHCP:
    // follow them now rather than at the next reboot.
    sacn::stop();
    fpp::stop();
    web::stop();
    net::apply(config::get_global());
    printf("note=network_applied\n");
    return ok();
}

int cmd_reboot(int, char**) {
    printf("OK\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

// Deliberate panic so the flash-coredump path can be exercised end to end
// (UART-only: physical access is the trust boundary, same as web_password).
int cmd_crash(int argc, char** argv) {
    if (argc != 2 || strcmp(argv[1], "confirm") != 0)
        return err("usage: crash confirm — abort() to test the coredump");
    printf("OK\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    abort();
}

// ── channel identify ────────────────────────────────────────────────────────

int cmd_audio(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "test") == 0)
        return audio::start_test() ? ok() : err("no sound: no speaker, volume off, or busy");
    uint32_t hz = 0, ms = 500;
    if (argc >= 3 && argc <= 4 && strcmp(argv[1], "tone") == 0 &&
        parse_u32_in(argv[2], 20, 8000, hz) && (argc == 3 || parse_u32_in(argv[3], 10, 5000, ms)))
        return audio::play_tone(static_cast<uint16_t>(hz), static_cast<uint16_t>(ms))
                 ? ok()
                 : err("no sound: no speaker, volume off, or busy");
    return err("usage: audio test | audio tone <20..8000 Hz> [10..5000 ms]");
}

int cmd_identify(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        dmx::identify_stop();
        return ok();
    }
    uint32_t blinks = dmx::kIdentifyBlinks;
    if (argc == 3 && !parse_u32_in(argv[2], 1, 60, blinks)) return err("blinks: 1..60");
    if (argc >= 2 && argc <= 3 &&
        strcmp(argv[1], "all") == 0) {  // every configured output, in turn
        const uint8_t mask = dmx::identify_configured_outputs();
        dmx::identify_outputs(mask, static_cast<uint8_t>(blinks));
        printf("identify=0x%02x x%u\n", mask, static_cast<unsigned>(blinks));
        return ok();
    }
    uint32_t ch = 0;
    if (argc < 2 || argc > 3 || !parse_u32_in(argv[1], 0, config::kNumChannels - 1, ch))
        return err("usage: identify <ch 0..7>|all [blinks] | identify stop");
    dmx::identify_start(ch, static_cast<uint8_t>(blinks));
    printf("identify=ch%u x%u\n", static_cast<unsigned>(ch), static_cast<unsigned>(blinks));
    return ok();
}

// ── effect bank ─────────────────────────────────────────────────────────────

const char* const kSceneFxNames[] = { "solid",   "chase", "rainbow", "blobs", "gradient", "fade",
                                      "twinkle", "fire",  "scanner", "wave",  "stripes" };
static_assert(sizeof(kSceneFxNames) / sizeof(kSceneFxNames[0]) == config::kSceneFxCount,
              "one console name per generator");

// "rrggbb[,rrggbb…]" → up to kSceneColorsMax colours; returns the count, 0 on error.
size_t parse_scene_colors(const char* arg, uint8_t out[][3]) {
    char buf[config::kSceneColorsMax * 7 + 1];
    copy_str(buf, sizeof(buf), arg);
    if (strlen(arg) >= sizeof(buf)) return 0;
    size_t n   = 0;
    char* save = nullptr;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        if (n == config::kSceneColorsMax || parse_hex(tok, out[n], 3) != 3) return 0;
        ++n;
    }
    return n;
}

bool parse_effect_index(const char* s, uint32_t& out) {
    return config::num_effects() > 0 && parse_u32_in(s, 0, config::num_effects() - 1, out);
}

int cmd_fx(int argc, char** argv) {
    if (argc == 1) {
        for (size_t i = 0; i < config::num_effects(); ++i) {
            const auto& e = config::get_effect(i);
            printf("fx%u name=%s generator=%s color=", static_cast<unsigned>(i), e.name,
                   kSceneFxNames[e.generator < config::kSceneFxCount ? e.generator : 0]);
            for (size_t k = 0; k < config::effect_num_colors(e); ++k)
                printf("%s%02x%02x%02x", k ? "," : "", e.colors[k][0], e.colors[k][1],
                       e.colors[k][2]);
            // phaser: wave,rate,spread,width,low[,reverse]
            // matricks: block,groups,wings
            // envelope: attack,decay (of a PWM phaser)
            printf(" speed=%u param=%u phaser=%s,%u,%u,%u,%u%s invert=%d matricks=%u,%u,%u "
                   "used=%d envelope=%u,%u\n",
                   e.speed, e.param, config::phaser_wave_id(e.ph_wave), e.ph_rate, e.ph_spread,
                   e.ph_width, e.ph_low, (e.flags & config::kEffectPhaserReverse) ? ",reverse" : "",
                   (e.flags & config::kEffectDimmerInvert) != 0, e.block, e.groups, e.wings,
                   config::effect_in_use(i), e.ph_attack, e.ph_decay);
        }
        return ok();
    }

    uint32_t n = 0;
    if (strcmp(argv[1], "name") == 0) {
        if (argc != 4 || !parse_effect_index(argv[2], n))
            return err("usage: fx name <index> <text>");
        config::ScopedLock lock;
        auto e = config::get_effect(n);
        copy_str(e.name, sizeof(e.name), argv[3]);
        if (!config::set_effect(n, e)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "set") == 0) {
        // fx set <n> <generator> <rrggbb[,rrggbb…]> <speed> <param>
        if (argc != 7)
            return err("usage: fx set <n> <generator> <rrggbb[,rrggbb...]> "
                       "<speed 0..255> <param 0..255>");
        if (!parse_effect_index(argv[2], n)) return err("fx: an existing index");
        const int gen = lookup_name(kSceneFxNames, config::kSceneFxCount, argv[3]);
        if (gen < 0)
            return err("generator: solid|chase|rainbow|blobs|gradient|fade|twinkle|fire|scanner|"
                       "wave|stripes or 0..10");
        uint8_t cols[config::kSceneColorsMax][3] = {};
        const size_t ncols                       = parse_scene_colors(argv[4], cols);
        if (ncols == 0) return err("colors: rrggbb[,rrggbb...] (1..4)");
        uint32_t speed = 0, param = 0;
        if (!parse_u32_in(argv[5], 0, 255, speed)) return err("speed: 0..255");
        if (!parse_u32_in(argv[6], 0, 255, param)) return err("param: 0..255");
        config::ScopedLock lock;
        auto e      = config::get_effect(n);
        e.generator = static_cast<uint8_t>(gen);
        memcpy(e.colors, cols, sizeof(e.colors));
        e.num_colors = static_cast<uint8_t>(ncols);
        e.speed      = static_cast<uint8_t>(speed);
        e.param      = static_cast<uint8_t>(param);
        if (!config::set_effect(n, e)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "phaser") == 0) {
        // fx phaser <n> <wave> [<rate> <spread> [<width> <low> [reverse|forward]]] — the
        // dimmer phaser; what is left out keeps its value.
        if (argc < 4 || argc == 5 || argc == 7 || argc > 9 || !parse_effect_index(argv[2], n))
            return err("usage: fx phaser <n> <wave> [<rate> <spread> [<width> <low> "
                       "[reverse|forward]]]");
        const int wave = config::phaser_wave_from_id(argv[3]);
        if (wave < 0) return err("wave: none|sin|cos|ramp_up|ramp_down|triangle|pwm|bump");
        uint32_t v[4] = {};
        for (int k = 4; k < argc && k < 8; ++k)
            if (!parse_u32_in(argv[k], 0, 255, v[k - 4]))
                return err("rate, spread, width, low: 0..255");
        const bool reverse = argc == 9 && strcmp(argv[8], "reverse") == 0;
        if (argc == 9 && !reverse && strcmp(argv[8], "forward") != 0)
            return err("direction: reverse|forward");
        config::ScopedLock lock;
        auto e    = config::get_effect(n);
        e.ph_wave = static_cast<uint8_t>(wave);
        if (argc >= 6) {
            e.ph_rate   = static_cast<uint8_t>(v[0]);
            e.ph_spread = static_cast<uint8_t>(v[1]);
        }
        if (argc >= 8) {
            e.ph_width = static_cast<uint8_t>(v[2]);
            e.ph_low   = static_cast<uint8_t>(v[3]);
        }
        if (argc == 9)
            e.flags = static_cast<uint8_t>(reverse ? e.flags | config::kEffectPhaserReverse
                                                   : e.flags & ~config::kEffectPhaserReverse);
        if (!config::set_effect(n, e)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "envelope") == 0) {
        // fx envelope <n> <attack> <decay> — a PWM phaser's fade in and out, each a
        // share of its lit part (0..255; 0 = a hard edge).
        uint32_t a = 0, d = 0;
        if (argc != 5 || !parse_effect_index(argv[2], n))
            return err("usage: fx envelope <n> <attack> <decay>");
        if (!parse_u32_in(argv[3], 0, 255, a) || !parse_u32_in(argv[4], 0, 255, d))
            return err("attack, decay: 0..255");
        config::ScopedLock lock;
        auto e      = config::get_effect(n);
        e.ph_attack = static_cast<uint8_t>(a);
        e.ph_decay  = static_cast<uint8_t>(d);
        if (!config::set_effect(n, e)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "matricks") == 0) {
        // fx matricks <n> <block> <groups> <wings> — on the pixels of the run; 0 = off
        uint32_t v[3] = {};
        if (argc != 6 || !parse_effect_index(argv[2], n))
            return err("usage: fx matricks <n> <block> <groups> <wings>");
        for (int k = 0; k < 3; ++k)
            if (!parse_u32_in(argv[3 + k], 0, 255, v[k]))
                return err("block, groups, wings: 0..255 (0 = off)");
        config::ScopedLock lock;
        auto e   = config::get_effect(n);
        e.block  = static_cast<uint8_t>(v[0]);
        e.groups = static_cast<uint8_t>(v[1]);
        e.wings  = static_cast<uint8_t>(v[2]);
        if (!config::set_effect(n, e)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "invert") == 0) {
        // fx invert <n> 0|1 — the intensity negative of the whole effect
        bool on = false;
        if (argc != 4 || !parse_effect_index(argv[2], n) || !parse_bool(argv[3], on))
            return err("usage: fx invert <n> 0|1");
        config::ScopedLock lock;
        auto e  = config::get_effect(n);
        e.flags = static_cast<uint8_t>(on ? e.flags | config::kEffectDimmerInvert
                                          : e.flags & ~config::kEffectDimmerInvert);
        if (!config::set_effect(n, e)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "add") == 0) {
        // fx add [name] — a solid white effect, appended.
        config::Effect e{};
        copy_str(e.name, sizeof(e.name), argc >= 3 ? argv[2] : "New effect");
        e.num_colors = 1;
        memset(e.colors[0], 255, 3);
        const int idx = config::add_effect(e);
        if (idx < 0) return err("effect bank full (31)");
        printf("index=%d\n", idx);
        return ok();
    }
    if (strcmp(argv[1], "del") == 0) {
        if (argc != 3 || !parse_effect_index(argv[2], n)) return err("usage: fx del <index>");
        if (!config::delete_effect(n)) return err("effect in use by a scene");
        return ok();
    }
    if (strcmp(argv[1], "move") == 0) {
        uint32_t to = 0;
        if (argc != 4 || !parse_effect_index(argv[2], n) || !parse_effect_index(argv[3], to))
            return err("usage: fx move <from> <to>");
        config::move_effect(n, to);
        return ok();
    }
    return err("usage: fx [name <n> <text> | set <n> ... | phaser <n> ... | invert <n> 0|1 | "
               "matricks <n> <block> <groups> <wings> | add [name] | del <n> | move <from> <to>]");
}

// ── standalone scenes ───────────────────────────────────────────────────────

bool parse_scene_index(const char* s, uint32_t& out) {
    return config::num_scenes() > 0 && parse_u32_in(s, 0, config::num_scenes() - 1, out);
}

int cmd_scene(int argc, char** argv) {
    if (argc == 1) {
        printf("active=%d\n", dmx::active_scene());
        printf("outputs=");  // scene per output, -1 = live
        for (size_t o = 0; o < config::kNumChannels; ++o)
            printf("%s%d", o ? "," : "", dmx::scene_on_output(o));
        printf("\n");
        for (size_t i = 0; i < config::num_scenes(); ++i) {
            const auto& sc = config::get_scene(i);
            // parts: <outputs-hex>:<effect>:<fixture mode>[:rev], comma-separated
            printf("scene%u name=%s mask=%02x group=%d parts=", static_cast<unsigned>(i), sc.name,
                   config::scene_mask(sc), config::scene_group(sc));
            for (size_t k = 0; k < sc.num_parts; ++k)
                printf("%s%02x:%u:%s%s", k ? "," : "", sc.parts[k].mask, sc.parts[k].effect,
                       config::fixture_mode_id(config::scene_mode_of(sc.parts[k].fixture_mode)),
                       config::scene_reverse_of(sc.parts[k].fixture_mode) ? ":rev" : "");
            printf("\n");
        }
        return ok();
    }

    uint32_t n = 0;
    if (strcmp(argv[1], "play") == 0 && argc == 5 && strcmp(argv[3], "group") == 0) {
        // scene play <n> group <g> — on that fixture group (0-based)
        uint32_t g = 0;
        if (!parse_scene_index(argv[2], n) || !parse_u32_in(argv[4], 0, 15, g) ||
            g >= config::get_groups().count)
            return err("usage: scene play <index> group <group 0..count-1>");
        dmx::group_play(static_cast<uint8_t>(n), static_cast<uint8_t>(g));
        printf("active=%u\ngroup=%u\n", static_cast<unsigned>(n), static_cast<unsigned>(g));
        return ok();
    }
    if (strcmp(argv[1], "play") == 0) {
        // scene play <n> [outputs-hex] — on the scene's mask (∩ outputs)
        uint8_t outs = dmx::kAllOutputs;
        if ((argc != 3 && argc != 4) || !parse_scene_index(argv[2], n) ||
            (argc == 4 && parse_hex(argv[3], &outs, 1) != 1))
            return err("usage: scene play <index> [outputs-hex]");
        dmx::scene_start_on(static_cast<uint8_t>(n), outs);
        printf("active=%u\n", static_cast<unsigned>(n));
        printf("outputs=%02x\n", dmx::scene_outputs(static_cast<uint8_t>(n)));
        return ok();
    }
    if (strcmp(argv[1], "stop") == 0) {
        // scene stop [n] — every output, or only those playing scene n
        if (argc == 3) {
            if (!parse_scene_index(argv[2], n)) return err("usage: scene stop [index]");
            dmx::scene_stop_scene(static_cast<uint8_t>(n));
        } else {
            dmx::scene_stop();
        }
        return ok();
    }
    if (strcmp(argv[1], "name") == 0) {
        if (argc != 4 || !parse_scene_index(argv[2], n))
            return err("usage: scene name <index> <text>");
        config::ScopedLock lock;
        auto sc = config::get_scene(n);
        copy_str(sc.name, sizeof(sc.name), argv[3]);
        if (!config::set_scene(n, sc)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "part") == 0) {
        // scene part <n> <outputs-hex> <effect> [mode] [rev] — those outputs play
        // that effect; they leave the part they were in. `rev`: from the far end.
        uint8_t mask[1];
        uint32_t fx    = 0;
        const bool rev = argc >= 6 && strcmp(argv[argc - 1], "rev") == 0;
        if (rev) --argc;
        if ((argc != 5 && argc != 6) || !parse_scene_index(argv[2], n))
            return err(
                "usage: scene part <n> <outputs-hex> <effect> [each|strip|chain|mirror] [rev]");
        if (parse_hex(argv[3], mask, 1) != 1 || mask[0] == 0)
            return err("outputs: 2-digit hex, 01..ff");
        if (!parse_effect_index(argv[4], fx)) return err("effect: an existing index (see `fx`)");
        int mode = config::kFixtureModeEach;
        if (argc == 6) {
            mode = -1;
            for (uint8_t m = 0; m < config::kFixtureModeCount; ++m)
                if (strcmp(argv[5], config::fixture_mode_id(m)) == 0) mode = m;
            if (mode < 0) return err("mode: each|strip|chain|mirror");
        }
        config::ScopedLock lock;
        auto sc = config::get_scene(n);
        for (size_t k = 0; k < sc.num_parts; ++k)
            sc.parts[k].mask = static_cast<uint8_t>(sc.parts[k].mask & ~mask[0]);
        config::sanitize_scene(sc);  // drops the parts left without an output
        if (sc.num_parts == config::kMaxSceneParts) return err("scene parts full (8)");
        sc.parts[sc.num_parts++] = config::ScenePart{
            mask[0], static_cast<uint8_t>(fx),
            static_cast<uint8_t>(mode | (rev ? config::kSceneReverseBit : 0)), 0
        };
        if (!config::set_scene(n, sc)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "group") == 0) {
        // scene group <n> <g|none> — the fixture group it plays on by default
        uint32_t g = 0;
        if (argc != 4 || !parse_scene_index(argv[2], n))
            return err("usage: scene group <index> <group 0..15|none>");
        const bool none = strcmp(argv[3], "none") == 0;
        if (!none && !parse_u32_in(argv[3], 0, config::kMaxGroups - 1, g))
            return err("group: 0..15, or none");
        config::ScopedLock lock;
        auto sc  = config::get_scene(n);
        sc.group = static_cast<uint8_t>(none ? 0 : g + 1);
        if (!config::set_scene(n, sc)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "clear") == 0) {
        // scene clear <n> — no part left: the scene plays nowhere
        if (argc != 3 || !parse_scene_index(argv[2], n)) return err("usage: scene clear <index>");
        config::ScopedLock lock;
        auto sc      = config::get_scene(n);
        sc.num_parts = 0;
        if (!config::set_scene(n, sc)) printf("warn=not_persisted\n");
        return ok();
    }
    if (strcmp(argv[1], "add") == 0) {
        // scene add [name] — the first effect on every output, appended.
        const int idx = config::add_scene(
            config::make_scene(argc >= 3 ? argv[2] : "New scene", 0xFF, 0));
        if (idx < 0) return err("scene list full (30)");
        printf("index=%d\n", idx);
        return ok();
    }
    if (strcmp(argv[1], "del") == 0) {
        if (argc != 3 || !parse_scene_index(argv[2], n)) return err("usage: scene del <index>");
        config::delete_scene(n);
        dmx::scene_list_edited(config::SceneEdit::Delete, n);
        return ok();
    }
    if (strcmp(argv[1], "move") == 0) {
        uint32_t to = 0;
        if (argc != 4 || !parse_scene_index(argv[2], n) || !parse_scene_index(argv[3], to))
            return err("usage: scene move <from> <to>");
        config::move_scene(n, to);
        dmx::scene_list_edited(config::SceneEdit::Move, n, to);
        return ok();
    }
    return err("usage: scene [play <n> | stop | name <n> <text> | part <n> ... | clear <n> | "
               "add [name] | del <n> | move <from> <to>]");
}

// ── fixture DMX profiles ────────────────────────────────────────────────────

// One slot as "fn", "fn:colour" (red:1 = colour 2) or "dimmer+fine".
bool parse_profile_slot(char* tok, config::ProfileSlot& out) {
    uint8_t arg = 0;
    if (char* plus = strchr(tok, '+')) {
        if (strcmp(plus, "+fine") != 0) return false;
        arg   |= config::kProfileArgFine;
        *plus  = '\0';
    }
    if (char* colon = strchr(tok, ':')) {
        uint32_t k = 0;
        if (!parse_u32_in(colon + 1, 0, config::kSceneColorsMax - 1, k)) return false;
        arg    |= static_cast<uint8_t>(k);
        *colon  = '\0';
    }
    const int fn = config::fix_fn_from_id(tok);
    if (fn < 0) return false;
    out = config::ProfileSlot{ static_cast<uint8_t>(fn), arg };
    return true;
}

void print_profiles(const config::ProfileBank& b) {
    printf("profiles=%u\n", b.count);
    for (size_t i = 0; i < b.count; ++i) {
        const auto& p = b.profiles[i];
        printf("profile%u name=%s footprint=%u slots=", static_cast<unsigned>(i), p.name,
               static_cast<unsigned>(config::profile_footprint(p)));
        for (size_t k = 0; k < p.count; ++k) {
            const auto& sl = p.slots[k];
            printf("%s%s", k ? "," : "", config::fix_fn_id(sl.fn));
            if (sl.arg & config::kProfileArgColor) printf(":%u", sl.arg & config::kProfileArgColor);
            if (sl.arg & config::kProfileArgFine) printf("+fine");
        }
        printf("\n");
    }
}

int cmd_profile(int argc, char** argv) {
    static config::ProfileBank b;  // 548 B: off the console task's stack
    config::ScopedLock lock;       // read-modify-write
    b = config::get_profiles();
    if (argc == 1) {
        print_profiles(b);
        return ok();
    }
    const char* sub = argv[1];
    uint32_t n      = 0;
    if (strcmp(sub, "add") == 0) {
        // profile add [name] — plain RGB, appended
        if (b.count >= config::kMaxProfiles) return err("profile bank full (8)");
        config::Profile& p = b.profiles[b.count];
        config::profile_apply_preset(p, config::ProfilePreset::Rgb);
        if (argc >= 3) copy_str(p.name, sizeof(p.name), argv[2]);
        printf("index=%u\n", b.count++);
    } else if (argc < 3 || !parse_u32_in(argv[2], 0, b.count - 1u, n)) {
        return err("usage: profile [add [name] | del <n> | name <n> <text> | "
                   "preset <n> rgb|dim_rgb|rgb_fx|full | slots <n> <fn[:colour][+fine],...>]");
    } else if (strcmp(sub, "del") == 0) {
        // The last profile stays: every fixture points at one.
        if (argc != 3 || b.count == 1) return err("usage: profile del <n> (one profile must stay)");
        for (size_t i = n; i + 1 < b.count; ++i)
            b.profiles[i] = b.profiles[i + 1];
        --b.count;
    } else if (strcmp(sub, "name") == 0) {
        if (argc != 4) return err("usage: profile name <n> <text>");
        copy_str(b.profiles[n].name, sizeof(b.profiles[n].name), argv[3]);
    } else if (strcmp(sub, "preset") == 0) {
        int preset = -1;
        for (uint8_t k = 0; argc == 4 && k < static_cast<uint8_t>(config::ProfilePreset::Count);
             ++k)
            if (strcmp(argv[3], config::profile_preset_id(static_cast<config::ProfilePreset>(k))) ==
                0)
                preset = k;
        if (preset < 0) return err("usage: profile preset <n> rgb|dim_rgb|rgb_fx|full");
        config::profile_apply_preset(b.profiles[n], static_cast<config::ProfilePreset>(preset));
    } else if (strcmp(sub, "slots") == 0) {
        if (argc != 4) return err("usage: profile slots <n> <fn[:colour][+fine],...>");
        config::Profile& p = b.profiles[n];
        static char list[config::kMaxProfileSlots * 16];
        if (strlen(argv[3]) >= sizeof(list)) return err("slots: too long");
        copy_str(list, sizeof(list), argv[3]);
        config::ProfileSlot slots[config::kMaxProfileSlots] = {};
        size_t count                                        = 0;
        char* save                                          = nullptr;
        for (char* tok = strtok_r(list, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
            if (count == config::kMaxProfileSlots) return err("slots: at most 24");
            if (!parse_profile_slot(tok, slots[count++]))
                return err(
                    "slot: dimmer[+fine]|red[:n]|green[:n]|blue[:n]|white|shutter|bank|"
                    "speed|param|ph_wave|ph_rate|ph_spread|ph_width|ph_attack|ph_decay|block|"
                    "groups|wings|fx_fade|none");
        }
        if (count == 0) return err("slots: at least one");
        memcpy(p.slots, slots, sizeof(slots));
        p.count = static_cast<uint8_t>(count);
    } else {
        return err("usage: profile [add [name] | del <n> | name <n> <text> | "
                   "preset <n> rgb|dim_rgb|rgb_fx|full | slots <n> <fn[:colour][+fine],...>]");
    }
    if (!config::set_profiles(b)) printf("warn=not_persisted\n");
    dmx::mark_global_dirty();  // footprints moved: the universe map follows
    print_profiles(config::get_profiles());
    return ok();
}

// ── FSEQ player ─────────────────────────────────────────────────────────────

// fseq playlist [play | clear | add <file> [repeat] | loop on|off | autostart on|off]
int cmd_fseq_playlist(int argc, char** argv) {
    config::FseqPlaylist p = config::get_playlist();
    if (argc == 1) {
        printf("count=%u loop=%u autostart=%u playing=%d\n", p.count, p.loop, p.autostart,
               fseq::playlist_index());
        for (size_t i = 0; i < p.count; ++i)
            printf("item%u=%s x%u\n", static_cast<unsigned>(i), p.items[i].name, p.items[i].repeat);
        return ok();
    }
    const char* sub = argv[1];
    if (strcmp(sub, "play") == 0 && argc == 2) {
        if (!fseq::start_playlist()) {
            return err(fseq::error_string());  // OK/ERR only, no non-zero code
        }
        return ok();
    }
    if (strcmp(sub, "clear") == 0 && argc == 2) {
        p.count = 0;
    } else if (strcmp(sub, "add") == 0 && (argc == 3 || argc == 4)) {
        if (p.count >= config::kPlaylistMax) return err("playlist full (16)");
        uint32_t repeat = 1;
        if (argc == 4 && !parse_u32_in(argv[3], 1, 255, repeat)) return err("repeat: 1..255");
        if (strlen(argv[2]) >= config::kPlaylistNameLen || strchr(argv[2], '/'))
            return err("bad filename");
        config::PlaylistItem& it = p.items[p.count++];
        it                       = config::PlaylistItem{};
        strncpy(it.name, argv[2], sizeof(it.name) - 1);
        it.repeat = static_cast<uint8_t>(repeat);
    } else if ((strcmp(sub, "loop") == 0 || strcmp(sub, "autostart") == 0) && argc == 3 &&
               (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        const uint8_t on                                  = strcmp(argv[2], "on") == 0;
        (strcmp(sub, "loop") == 0 ? p.loop : p.autostart) = on;
    } else {
        return err("usage: fseq playlist [play | clear | add <file> [repeat] | loop on|off | "
                   "autostart on|off]");
    }
    config::set_playlist(p);
    printf("count=%u loop=%u autostart=%u\n", config::get_playlist().count,
           config::get_playlist().loop, config::get_playlist().autostart);
    return ok();
}

int cmd_fseq(int argc, char** argv) {
    if (argc == 1) {
        const char* f = fseq::active_file();
        // One key per line, as every other command: tools parse key=value lines.
        printf("status=%s\nactive=%s\n",
               fseq::status() == fseq::Status::Playing ? "playing" : "idle", f ? f : "none");
        if (fseq::status() == fseq::Status::Playing)
            printf("position_ms=%u\nduration_ms=%u\n", static_cast<unsigned>(fseq::position_ms()),
                   static_cast<unsigned>(fseq::duration_ms()));
        if (fseq::status() == fseq::Status::Error) printf("error=%s\n", fseq::error_string());
        // List available files
        char names[fseq::kMaxFiles][fseq::kMaxNameLen];
        const size_t n = fseq::list_files(names, fseq::kMaxFiles);
        for (size_t i = 0; i < n; ++i)
            printf("file%u=%s\n", static_cast<unsigned>(i), names[i]);
        return ok();
    }
    if (strcmp(argv[1], "play") == 0) {
        const bool loop = argc == 4 && strcmp(argv[3], "loop") == 0;
        if (argc != 3 && !loop) return err("usage: fseq play <filename> [loop]");
        if (!fseq::start(argv[2], loop)) {
            return err(fseq::error_string());  // OK/ERR only, no non-zero code
        }
        printf("active=%s\n", argv[2]);
        return ok();
    }
    if (strcmp(argv[1], "stop") == 0) {
        fseq::stop();
        return ok();
    }
    if (strcmp(argv[1], "seek") == 0) {
        uint32_t ms = 0;
        if (argc != 3 || !parse_u32(argv[2], ms)) return err("usage: fseq seek <ms>");
        if (!fseq::seek_ms(ms)) return err("nothing playing");
        printf("position_ms=%u\n", static_cast<unsigned>(ms));
        return ok();
    }
    if (strcmp(argv[1], "list") == 0) {
        char names[fseq::kMaxFiles][fseq::kMaxNameLen];
        const size_t n = fseq::list_files(names, fseq::kMaxFiles);
        printf("count=%u\n", static_cast<unsigned>(n));
        for (size_t i = 0; i < n; ++i)
            printf("file%u=%s\n", static_cast<unsigned>(i), names[i]);
        return ok();
    }
    if (strcmp(argv[1], "playlist") == 0) return cmd_fseq_playlist(argc - 1, argv + 1);
    return err("usage: fseq [list | play <filename> [loop] | stop | seek <ms> | playlist ...]");
}

// ── show / ctrl ─────────────────────────────────────────────────────────────

// Optional trailing outputs mask (hex); all outputs when absent.
bool parse_outputs(int argc, char** argv, int at, uint8_t& out) {
    out = dmx::kAllOutputs;
    if (argc <= at) return true;
    return argc == at + 1 && parse_hex(argv[at], &out, 1) == 1;
}

// show — grand master / blackout / strobe / scene fade (+ control state)
int cmd_show(int argc, char** argv) {
    uint32_t v   = 0;
    uint8_t outs = dmx::kAllOutputs;
    if (argc == 1) {
        printf("master=");
        for (size_t o = 0; o < config::kNumChannels; ++o)
            printf("%s%u", o ? "," : "",
                   (static_cast<unsigned>(dmx::master_effective(o)) * 100u + 32767u) / 65535u);
        printf("\nmaster_local=");
        for (size_t o = 0; o < config::kNumChannels; ++o)
            printf("%s%u", o ? "," : "",
                   (static_cast<unsigned>(dmx::master_local(o)) * 100u + 32767u) / 65535u);
        printf("\nblackout=%02x\nblackout_local=%02x\n", dmx::blackout_effective(),
               dmx::blackout_local());
        printf("strobe_hz10=");
        for (size_t o = 0; o < config::kNumChannels; ++o)
            printf("%s%u", o ? "," : "", dmx::strobe_effective(o));
        printf("\nfade_ms=%lu\nfade_ms_config=%u\n",
               static_cast<unsigned long>(dmx::scene_fade_ms()),
               config::get_global().scene_fade_ms);
        printf("control=%s\n", !config::get_control().enabled ? "off"
                               : dmx::control_live()          ? "live"
                                                              : "idle");
        return ok();
    }
    if (strcmp(argv[1], "master") == 0) {
        if (argc < 3 || !parse_u32_in(argv[2], 0, 100, v) || !parse_outputs(argc, argv, 3, outs))
            return err("usage: show master <0..100> [outputs-hex]");
        dmx::master_set(outs, static_cast<uint16_t>(v * 65535u / 100u));
        return ok();
    }
    if (strcmp(argv[1], "blackout") == 0) {
        if (argc < 3 || !parse_outputs(argc, argv, 3, outs))
            return err("usage: show blackout on|off|toggle [outputs-hex]");
        if (strcmp(argv[2], "on") == 0)
            dmx::blackout_set(outs, true);
        else if (strcmp(argv[2], "off") == 0)
            dmx::blackout_set(outs, false);
        else if (strcmp(argv[2], "toggle") == 0)
            dmx::blackout_toggle(outs);
        else
            return err("usage: show blackout on|off|toggle [outputs-hex]");
        printf("blackout=%02x\n", dmx::blackout_effective());
        return ok();
    }
    if (strcmp(argv[1], "strobe") == 0) {
        if (argc < 3 || !parse_u32_in(argv[2], 0, 25, v) || !parse_outputs(argc, argv, 3, outs))
            return err("usage: show strobe <0..25 Hz> [outputs-hex]");
        dmx::strobe_set(outs, static_cast<uint8_t>(v * 10));
        return ok();
    }
    if (strcmp(argv[1], "fade") == 0) {
        if (argc != 3 || !parse_u32_in(argv[2], 0, config::kMaxSceneFadeMs, v))
            return err("usage: show fade <0..25500 ms>");
        auto g          = config::get_global();
        g.scene_fade_ms = static_cast<uint16_t>(v);
        if (!config::set_global(g)) printf("warn=not_persisted\n");
        return ok();
    }
    return err("usage: show [master|blackout|strobe|fade ...]");
}

void print_control(const config::ControlConfig& c) {
    printf("enabled=%u\nuniverse=%u\naddress=%u\nslots=%u\nfootprint=%u\nlive=%d\n", c.enabled,
           c.universe, c.address, c.count, static_cast<unsigned>(config::control_footprint(c)),
           dmx::control_live() ? 1 : 0);
    printf("pool_slot=%d\nlast_rx_ms=%lld\n", dmx::control_pool_slot(),
           static_cast<long long>(dmx::control_last_rx_ms_ago()));
    unsigned at = c.address;
    for (size_t i = 0; i < c.count; ++i) {
        const auto& s    = c.slots[i];
        const unsigned w = config::control_slot_width(s);
        const int g      = config::control_slot_group(s);
        char target[12];
        if (g >= 0)
            snprintf(target, sizeof(target), "group=%d", g);  // a fixture group
        else
            snprintf(target, sizeof(target), "mask=%02x", s.mask);
        printf("slot%u dmx=%u%s fn=%s %s index=%u fine=%u\n", static_cast<unsigned>(i), at,
               w == 2 ? "+1" : "", config::ctl_fn_id(s.fn), target, s.index,
               (s.flags & config::kCtlFlagFine) ? 1u : 0u);
        at += w;
    }
}

// <fn> [mask-hex] [index] [fine] → slot
bool parse_ctl_slot(int argc, char** argv, int at, config::ControlSlot& out) {
    if (argc <= at) return false;
    const int fn = config::ctl_fn_from_id(argv[at]);
    if (fn < 0) return false;
    out        = config::control_slot(static_cast<config::CtlFn>(fn));
    uint32_t v = 0;
    // The target: outputs as a hex mask, or g<n> for fixture group n (0-based).
    bool group = false;
    if (argc > at + 1 && argv[at + 1][0] == 'g') {
        if (!parse_u32_in(argv[at + 1] + 1, 0, config::kMaxGroups - 1, v)) return false;
        out.mask = static_cast<uint8_t>(v);
        group    = true;
    } else if (argc > at + 1 && parse_hex(argv[at + 1], &out.mask, 1) != 1) {
        return false;
    }
    if (argc > at + 2) {
        if (!parse_u32_in(argv[at + 2], 0, config::kSceneColorsMax - 1, v)) return false;
        out.index = static_cast<uint8_t>(v);
    }
    if (argc > at + 3) {
        if (!parse_u32_in(argv[at + 3], 0, 1, v)) return false;
        out.flags = v ? config::kCtlFlagFine : 0;
    }
    if (group) out.flags |= config::kCtlFlagGroup;
    return argc <= at + 4;
}

int apply_control(const config::ControlConfig& c) {
    if (!config::set_control(c)) printf("warn=not_persisted\n");
    dmx::mark_global_dirty();  // maps (or drops) the control universe
    print_control(config::get_control());
    return ok();
}

// ctrl — the DMX control universe ("personality")
int cmd_ctrl(int argc, char** argv) {
    config::ControlConfig c = config::get_control();
    uint32_t v              = 0;
    if (argc == 1) {
        print_control(c);
        return ok();
    }
    const char* sub = argv[1];
    if (strcmp(sub, "enable") == 0) {
        if (argc != 3 || !parse_u32_in(argv[2], 0, 1, v)) return err("usage: ctrl enable 0|1");
        c.enabled = static_cast<uint8_t>(v);
    } else if (strcmp(sub, "universe") == 0) {
        if (argc != 3 || !parse_u32_in(argv[2], 0, 32767, v))
            return err("usage: ctrl universe <0..32767>");
        c.universe = static_cast<uint16_t>(v);
    } else if (strcmp(sub, "address") == 0) {
        if (argc != 3 || !parse_u32_in(argv[2], 1, 512, v))
            return err("usage: ctrl address <1..512>");
        c.address = static_cast<uint16_t>(v);
    } else if (strcmp(sub, "preset") == 0) {
        if (argc == 3 && strcmp(argv[2], "simple") == 0)
            config::control_apply_preset(c, config::ControlPreset::Simple);
        else if (argc == 3 && strcmp(argv[2], "full") == 0)
            config::control_apply_preset(c, config::ControlPreset::Full);
        else
            return err("usage: ctrl preset simple|full");
    } else if (strcmp(sub, "clear") == 0) {
        c.count = 0;
    } else if (strcmp(sub, "add") == 0) {
        config::ControlSlot s{};
        if (!parse_ctl_slot(argc, argv, 2, s))
            return err("usage: ctrl add <fn> [mask-hex|g<group>] [colour 0..3] [fine 0|1]");
        if (c.count >= config::kMaxControlSlots) return err("control mode full (32 slots)");
        c.slots[c.count++] = s;
    } else if (strcmp(sub, "set") == 0) {
        config::ControlSlot s{};
        if (argc < 4 || !parse_u32_in(argv[2], 0, c.count ? c.count - 1u : 0u, v) || !c.count ||
            !parse_ctl_slot(argc, argv, 3, s))
            return err("usage: ctrl set <slot> <fn> [mask-hex|g<group>] [colour 0..3] [fine 0|1]");
        c.slots[v] = s;
    } else if (strcmp(sub, "del") == 0) {
        if (argc != 3 || !c.count || !parse_u32_in(argv[2], 0, c.count - 1u, v))
            return err("usage: ctrl del <slot>");
        for (size_t i = v; i + 1 < c.count; ++i)
            c.slots[i] = c.slots[i + 1];
        --c.count;
    } else {
        return err("usage: ctrl [enable|universe|address|preset|clear|add|set|del ...]");
    }
    const size_t before         = c.count;
    config::ControlConfig check = c;
    config::sanitize_control(check);
    if (check.count < before) return err("mode would end past DMX channel 512");
    return apply_control(c);
}

void register_cmd(const char* name, const char* help, esp_console_cmd_func_t fn) {
    const esp_console_cmd_t cmd = {
        .command        = name,
        .help           = help,
        .hint           = nullptr,
        .func           = fn,
        .argtable       = nullptr,
        .func_w_context = nullptr,
        .context        = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace

void start() {
    esp_console_repl_t* repl           = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt                    = "pixfrog>";
    // A full-universe dmxw is "dmxw 32767 1 " + 1024 hex chars ≈ 1040 bytes.
    repl_cfg.max_cmdline_length = 1152;
    repl_cfg.task_stack_size    = 8192;

    // The board's USB port is a USB-UART bridge into UART0 (the RTS→EN reset
    // circuit gives it away), so the REPL rides the default UART console.
    esp_console_dev_uart_config_t hw_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&hw_cfg, &repl_cfg, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "UART REPL init failed");
        return;
    }

    esp_console_register_help_command();
    register_cmd("version", "Firmware/IDF version", cmd_version);
    register_cmd("rollback", "Acknowledge the recorded OTA rollback: rollback ack", cmd_rollback);
    register_cmd("status", "Link, IP, MAC, FPS, cal mode, heap", cmd_status);
    register_cmd("stats", "DMX/ArtNet/DMA telemetry counters", cmd_stats);
    register_cmd("tasks", "Each task's share of a core over half a second (like top)", cmd_tasks);
    register_cmd("chstat", "Per-channel activity + capacity flags", cmd_chstat);
    register_cmd("global", "global [<key> <value>] — get/set GlobalConfig", cmd_global);
    register_cmd("ch", "ch <n> [<key> <value>] — get/set ChannelConfig", cmd_ch);
    register_cmd(
        "autopatch",
        "autopatch <base> [compact] [continuous|whole|fixture|colour] — re-address all channels",
        cmd_autopatch);
    register_cmd("overlaps", "overlaps — the patch's ranges that share DMX channels", cmd_overlaps);
    register_cmd("dmxw", "dmxw <universe> <start_slot> <hex> — inject DMX data", cmd_dmxw);
    register_cmd("dmxr", "dmxr <universe> [start len] — read universe buffer", cmd_dmxr);
    register_cmd("pixr", "pixr <ch> [start len] — read decoded pixel buffer", cmd_pixr);
    register_cmd("identify", "identify <ch>|all [blinks] — blink strips white to locate them",
                 cmd_identify);
    register_cmd("audio", "audio test | audio tone <Hz> [ms] — the speaker", cmd_audio);
    register_cmd("fx", "fx [name|set|phaser|invert|matricks|add|del|move] — the effect bank",
                 cmd_fx);
    register_cmd(
        "scene",
        "scene [play <n> [outputs|group <g>]|stop [n]|name|part|group|clear] — standalone scenes",
        cmd_scene);
    register_cmd("profile", "profile [add|del|name|preset|slots] — fixture DMX profiles",
                 cmd_profile);
    register_cmd("show", "show [master|blackout|strobe|fade] — grand master & show control",
                 cmd_show);
    register_cmd("ctrl", "ctrl [enable|universe|address|preset|add|set|del|clear] — DMX control",
                 cmd_ctrl);
    register_cmd(
        "fseq",
        "fseq [list | play <file> [loop] | stop | seek <ms> | playlist ...] — FSEQ show player",
        cmd_fseq);
    register_cmd("cal", "cal [-1|0|1|2|3] — get/set calibration pattern (3 = GPIO bit-bang probe)",
                 cmd_cal);
    register_cmd("loglevel", "loglevel <none..verbose> — set global log level", cmd_loglevel);
    register_cmd("factory-reset", "Restore default config (no reboot)", cmd_factory_reset);
    register_cmd("reboot", "Restart the device", cmd_reboot);
    register_cmd("crash", "crash confirm — deliberate abort() to test the coredump", cmd_crash);

    if (esp_console_start_repl(repl) != ESP_OK) {
        ESP_LOGE(TAG, "REPL start failed");
        return;
    }
    ESP_LOGI(TAG, "control console up on UART0");
}

}  // namespace pixfrog::console
