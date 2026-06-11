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
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "lcd_cam_output.h"
#include "led_protocols.h"
#include "ui.h"
#include "web_config.h"

namespace pixfrog::console {

namespace {

constexpr const char* TAG = "CONSOLE";

const char* const kProtocolNames[] = { "Off",    "WS2815", "WS2812B", "WS2811",  "SK6812",
                                       "WS2814", "APA102", "SK9822",  "LPD8806", "DMX512" };
static_assert(sizeof(kProtocolNames) / sizeof(kProtocolNames[0]) ==
              static_cast<size_t>(led::Protocol::COUNT));

const char* const kOrderNames[] = { "RGB", "RBG",  "GRB",  "GBR",  "BRG",
                                    "BGR", "RGBW", "GRBW", "RGBWW" };
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
    printf("cal=%d\n", static_cast<int>(lcd::get_calibration_mode()));
    printf("persist_ok=%d\n", config::is_persistence_ok() ? 1 : 0);
    printf("fb_bytes=%u\n", static_cast<unsigned>(lcd::fb_bytes()));
    printf("heap_free=%u\n", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    printf("psram_free=%u\n", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ok();
}

int cmd_stats(int, char**) {
    const dmx::Stats s = dmx::get_stats();
    printf("frames_emitted=%llu\n", static_cast<unsigned long long>(s.frames_emitted));
    printf("artnet_packets_rx=%llu\n", static_cast<unsigned long long>(s.artnet_packets_rx));
    printf("artnet_bad_packets=%llu\n", static_cast<unsigned long long>(s.artnet_bad_packets));
    printf("dma_underruns=%lu\n", static_cast<unsigned long>(s.dma_underruns));
    printf("current_fps=%lu\n", static_cast<unsigned long>(s.current_fps));
    const lcd::DebugCounters d = lcd::get_debug_counters();
    printf("lcd_trans_done=%lu\n", static_cast<unsigned long>(d.trans_done));
    printf("lcd_vsync=%lu\n", static_cast<unsigned long>(d.vsync));
    printf("lcd_msync_err=%lu\n", static_cast<unsigned long>(d.msync_err));
    return ok();
}

int cmd_chstat(int, char**) {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& c = config::get_channel(ch);
        printf("ch%u protocol=%s universe=%u pixels=%u active=%d capacity_ok=%d\n",
               static_cast<unsigned>(ch), kProtocolNames[static_cast<size_t>(c.protocol)],
               c.universe_start, c.pixel_count, dmx::is_channel_active(ch) ? 1 : 0,
               dmx::is_channel_capacity_ok(ch) ? 1 : 0);
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
    printf("web_enabled=%d\n", g.web_enabled ? 1 : 0);
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
        if (!parse_u32(val, u) || (u != 30 && u != 60)) return err("refresh_hz: 30|60");
        g.refresh_rate_hz = static_cast<uint8_t>(u);
    } else if (strcmp(key, "home_timeout_s") == 0) {
        if (!parse_u32_in(val, 0, 65535, u)) return err("home_timeout_s: 0..65535");
        g.home_timeout_s = static_cast<uint16_t>(u);
    } else if (strcmp(key, "web_enabled") == 0) {
        if (!parse_bool(val, g.web_enabled)) return err("web_enabled: 0|1");
    } else {
        return err("unknown key (dhcp ip mask gw net subnet short_name long_name reply_unicast "
                   "refresh_hz home_timeout_s web_enabled)");
    }

    const bool persisted = config::set_global(g);
    dmx::mark_global_dirty();
    if (!persisted) printf("warn=not_persisted\n");
    if (network_changed) printf("note=network_changes_apply_after_reboot\n");

    // Apply web server state immediately (does not require reboot).
    if (strcmp(key, "web_enabled") == 0) {
        if (g.web_enabled)
            web::start();
        else
            web::stop();
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
                       "LPD8806|DMX512 or 0..9");
        c.protocol = static_cast<led::Protocol>(p);
    } else if (strcmp(key, "order") == 0) {
        const int o = lookup_name(kOrderNames, static_cast<size_t>(led::ColorOrder::COUNT), val);
        if (o < 0) return err("order: RGB|RBG|GRB|GBR|BRG|BGR|RGBW|GRBW|RGBWW or 0..8");
        c.color_order = static_cast<led::ColorOrder>(o);
    } else if (strcmp(key, "universe") == 0) {
        if (!parse_u32_in(val, 1, 32767, u)) return err("universe: 1..32767");
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
    } else if (strcmp(key, "clock_hz") == 0) {
        if (!parse_u32_in(val, 100'000, led::kPclkHz / 2, u))
            return err("clock_hz: 100000..8000000");
        c.clock_hz = u;
    } else {
        return err("unknown key (protocol order universe dmx_start pixels brightness grouping "
                   "invert clock_hz)");
    }

    const bool persisted = config::set_channel(ch, c);
    dmx::mark_channel_dirty(ch);
    if (!persisted) printf("warn=not_persisted\n");
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
        printf("cal=%d\n", static_cast<int>(lcd::get_calibration_mode()));
        return ok();
    }
    if (argc != 2) return err("usage: cal [-1|0|1|2|3]");
    char* end    = nullptr;
    const long v = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || v < -1 || v > 3) return err("mode: -1..3");
    lcd::set_calibration_mode(static_cast<int8_t>(v));
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
    printf("note=reboot_recommended\n");
    return ok();
}

int cmd_reboot(int, char**) {
    printf("OK\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
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
    register_cmd("status", "Link, IP, MAC, FPS, cal mode, heap", cmd_status);
    register_cmd("stats", "DMX/ArtNet/DMA telemetry counters", cmd_stats);
    register_cmd("chstat", "Per-channel activity + capacity flags", cmd_chstat);
    register_cmd("global", "global [<key> <value>] — get/set GlobalConfig", cmd_global);
    register_cmd("ch", "ch <n> [<key> <value>] — get/set ChannelConfig", cmd_ch);
    register_cmd("dmxw", "dmxw <universe> <start_slot> <hex> — inject DMX data", cmd_dmxw);
    register_cmd("dmxr", "dmxr <universe> [start len] — read universe buffer", cmd_dmxr);
    register_cmd("pixr", "pixr <ch> [start len] — read decoded pixel buffer", cmd_pixr);
    register_cmd("cal", "cal [-1|0|1|2|3] — get/set calibration pattern (3 = GPIO bit-bang probe)",
                 cmd_cal);
    register_cmd("loglevel", "loglevel <none..verbose> — set global log level", cmd_loglevel);
    register_cmd("factory-reset", "Restore default config (no reboot)", cmd_factory_reset);
    register_cmd("reboot", "Restart the device", cmd_reboot);

    if (esp_console_start_repl(repl) != ESP_OK) {
        ESP_LOGE(TAG, "REPL start failed");
        return;
    }
    ESP_LOGI(TAG, "control console up on UART0");
}

}  // namespace pixfrog::console
