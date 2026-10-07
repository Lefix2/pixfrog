#include "artnet.h"
#include "artnet_parser.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "fseq_player.h"
#include "led_protocols.h"
#include "net.h"

namespace pixfrog::artnet {

namespace {

constexpr const char* TAG = "ARTNET";

// Every pool universe plus the control universe.
constexpr size_t kMaxBinds = dmx::kNumUniverses + 1;

TaskHandle_t g_task = nullptr;
int g_sock          = -1;
bool g_run          = false;
uint32_t g_local_ip = 0;

void handle_dmx(const uint8_t* buf, size_t len, const sockaddr_in& from) {
    parser::DmxFields f{};
    if (!parser::parse_dmx(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }

    // universe_start is the absolute 15-bit Art-Net Port-Address; routing is an
    // exact LUT match, so the LUT itself drops Art-Net meant for other nodes.
    // There is no node-wide Net/Sub-Net: a patch may span several subnets.

    // 2-source merge keyed by sender IP; a third concurrent sender is dropped.
    if (!dmx::write_universe_from_source(f.universe, f.data, f.data_len, from.sin_addr.s_addr,
                                         dmx::kArtnetMergeTimeoutUs)) {
        return;  // universe not mapped to any channel — silently drop
    }
    dmx::note_packet_rx();

    dmx::note_universe_activity(f.universe);  // every output it feeds
}

// One bind per universe the box listens to, each with its own Net/Sub-Net
// (Art-Net 4: ports on several subnets take a BindIndex each). Bind 1 is the
// first universe of the patch; a box with nothing patched still answers once,
// with no port, so the desk finds it.
size_t listened_universes(uint16_t out[kMaxBinds]) {
    return dmx::listened_universes(out, kMaxBinds);
}

void send_poll_reply(uint32_t target_addr_net_order) {
    if (g_sock < 0 || g_local_ip == 0) return;

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_ETH);

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = target_addr_net_order;
    dst.sin_port        = htons(kArtnetPort);

    uint16_t unis[kMaxBinds];
    const size_t n = listened_universes(unis);

    uint8_t pkt[parser::kPollReplySize];
    char node_report[64];

    const auto& g = config::get_global();
    for (size_t i = 0; i < (n ? n : 1); ++i) {
        const auto bind = static_cast<uint8_t>(i + 1);
        std::snprintf(node_report, sizeof(node_report), "#0001 [%u] pixfrog OK",
                      static_cast<unsigned>(bind));

        parser::PollReplyInputs in{};
        in.local_ip_host = g_local_ip;
        in.short_name    = g.short_name;
        in.long_name     = g.long_name;
        in.node_report   = node_report;
        in.mac           = mac;
        in.bind_index    = bind;
        in.merge_ltp     = g.merge_mode == config::kMergeLtp;
        in.num_ports     = n ? 1 : 0;
        if (n) {
            const uint16_t u   = unis[i];
            in.artnet_net      = static_cast<uint8_t>((u >> 8) & 0x7F);
            in.artnet_subnet   = static_cast<uint8_t>((u >> 4) & 0x0F);
            in.sw_out[0]       = static_cast<uint8_t>(u & 0x0F);
            in.port_merging[0] = dmx::is_universe_merging(u);
        }

        parser::build_poll_reply(pkt, in);
        int n_sent = sendto(g_sock, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dst),
                            sizeof(dst));
        if (n_sent < 0) {
            ESP_LOGW(TAG, "ArtPollReply bind %u sendto failed", bind);
            return;
        }
    }
}

// ArtAddress re-addresses a bind's universe: the range of every output that
// starts on it (pixels, fixtures) and the control universe move with it. A
// universe inside a range is not a start of its own and is left alone.
void readdress_universe(uint16_t from, uint16_t to) {
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c = config::get_channel(ch);
        if (led::is_off(c.protocol)) continue;
        bool moved = false;
        if (c.universe_start == from && dmx::channel_pixel_span(c) > 0) {
            c.universe_start = to;
            moved            = true;
        }
        if (config::fix_universe(c) == from && dmx::channel_fixture_span(c) > 0) {
            config::set_fix_address(c, to, config::fix_dmx_start(c));
            moved = true;
        }
        if (!moved) continue;
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    auto ctrl = config::get_control();
    if (ctrl.enabled && ctrl.universe == from) {
        ctrl.universe = to;
        config::set_control(ctrl);
        dmx::mark_global_dirty();
    }
}

void handle_poll(const uint8_t* /*buf*/, size_t len, const sockaddr_in& from) {
    if (len < 14) {
        dmx::note_packet_bad();
        return;
    }
    // Choose broadcast (default) vs unicast back to the poller's IP.
    const auto& g         = config::get_global();
    const uint32_t target = g.artnet_poll_reply_unicast
                              ? from.sin_addr.s_addr  // already network-order
                              : htonl(INADDR_BROADCAST);
    send_poll_reply(target);
}

void handle_sync(const uint8_t* /*buf*/, size_t /*len*/) {
    dmx::note_sync();
}

// ArtAddress — remote programming from the desk: names, the bind's universe
// (NetSwitch, SubSwitch, SwOut of its one port), merge mode. Applied fields
// persist to NVS; always answer with an ArtPollReply to the sender so it sees
// the new state.
void handle_address(const uint8_t* buf, size_t len, const sockaddr_in& from) {
    parser::AddressFields f{};
    if (!parser::parse_address(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();

    {
        // Read-modify-write of the global and channel configs: hold the config
        // lock so a web/console/UI edit cannot land in between and be lost.
        config::ScopedLock lock;
        config::GlobalConfig g = config::get_global();
        bool global_changed    = false;

        if (f.short_name[0] != '\0') {
            std::memset(g.short_name, 0, sizeof(g.short_name));
            std::memcpy(g.short_name, f.short_name, sizeof(g.short_name) - 1);
            global_changed = true;
        }
        if (f.long_name[0] != '\0') {
            std::memset(g.long_name, 0, sizeof(g.long_name));
            std::memcpy(g.long_name, f.long_name, sizeof(g.long_name) - 1);
            global_changed = true;
        }

        // BindIndex 0 is the root device: bind 1 (see send_poll_reply).
        uint16_t unis[kMaxBinds];
        const size_t n      = listened_universes(unis);
        const size_t bind   = f.bind_index <= 1 ? 1 : f.bind_index;
        const bool programs = (f.net_switch | f.sub_switch | f.sw_out[0]) & 0x80;
        if (programs && bind <= n) {
            const uint16_t was = unis[bind - 1];
            uint16_t to        = was;
            if (f.net_switch & 0x80)
                to = static_cast<uint16_t>((to & 0x00FF) | ((f.net_switch & 0x7F) << 8));
            if (f.sub_switch & 0x80)
                to = static_cast<uint16_t>((to & 0x7F0F) | ((f.sub_switch & 0x0F) << 4));
            if (f.sw_out[0] & 0x80)
                to = static_cast<uint16_t>((to & 0x7FF0) | (f.sw_out[0] & 0x0F));
            if (to != was) readdress_universe(was, to);
        }

        // Merge commands. The spec scopes AcMerge* to one port; pixfrog applies a
        // single node-wide merge mode, so any port's command switches it globally.
        if (f.command == parser::kAcCancelMerge) {
            dmx::merge_cancel_all();
            ESP_LOGI(TAG, "ArtAddress: merge cancelled");
        } else if (parser::is_merge_ltp_command(f.command) ||
                   parser::is_merge_htp_command(f.command)) {
            const uint8_t mode = parser::is_merge_ltp_command(f.command) ? config::kMergeLtp
                                                                         : config::kMergeHtp;
            if (g.merge_mode != mode) {
                g.merge_mode   = mode;
                global_changed = true;
            }
            ESP_LOGI(TAG, "ArtAddress: merge mode %s", mode == config::kMergeLtp ? "LTP" : "HTP");
        } else if (f.command != parser::kAcNone) {
            ESP_LOGI(TAG, "ArtAddress command 0x%02X ignored", f.command);
        }

        if (global_changed) {
            config::set_global(g);
            dmx::mark_global_dirty();
        }
    }

    send_poll_reply(from.sin_addr.s_addr);
}

// ArtIpProg — remote IP configuration. Same constraint as the UI/console
// paths: network changes apply after reboot. Always answer ArtIpProgReply
// with the *configured* values.
void handle_ip_prog(const uint8_t* buf, size_t len, const sockaddr_in& from) {
    parser::IpProgFields f{};
    if (!parser::parse_ip_prog(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();

    config::GlobalConfig g;
    {
        config::ScopedLock lock;  // read-modify-write, see handle_address
        g = config::get_global();
        if (f.command & 0x80) {  // programming enabled
            if (f.command & 0x10) {
                // Reset to defaults: DHCP on, static fields cleared.
                g.use_dhcp       = true;
                g.static_ip      = 0;
                g.static_mask    = 0;
                g.static_gateway = 0;
            } else {
                if (f.command & 0x40) g.use_dhcp = true;
                if (f.command & 0x04) {
                    g.static_ip = f.prog_ip;
                    g.use_dhcp  = false;
                }
                if (f.command & 0x02) g.static_mask = f.prog_mask;
                if (f.command & 0x08) g.static_gateway = f.prog_gw;
            }
            config::set_global(g);
            dmx::mark_global_dirty();
            ESP_LOGI(TAG, "ArtIpProg applied (cmd 0x%02X)", f.command);
        }
    }
    // The desk expects the node on its new address right away.
    if (f.command & 0x80) net::apply(g);

    if (g_sock < 0) return;
    uint8_t pkt[parser::kIpProgReplySize];
    parser::IpProgReplyInputs in{};
    in.ip           = g.use_dhcp ? g_local_ip : g.static_ip;
    in.mask         = g.static_mask;
    in.gw           = g.static_gateway;
    in.dhcp_enabled = g.use_dhcp;
    parser::build_ip_prog_reply(pkt, in);

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = from.sin_addr.s_addr;
    dst.sin_port        = htons(kArtnetPort);
    sendto(g_sock, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

// ArtNzs — non-zero-start-code DMX. Validated and counted; the payload is
// NOT routed: the universe pool stores start-code-0 levels only.
void handle_nzs(const uint8_t* buf, size_t len) {
    parser::NzsFields f{};
    if (!parser::parse_nzs(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();
}

// ArtTrigger — show control from the desk. Global packets (Oem 0xFFFF):
// Key 3 (KeyShow) drives the standalone scenes — SubKey 0 stops them all,
// SubKey N plays scene N-1 on its own outputs; Key 1 (KeyMacro) drives the
// blackout — SubKey 1 toggles, 2 = on, 3 = off. Other keys/Oems are counted
// but ignored.
void handle_trigger(const uint8_t* buf, size_t len) {
    if (len < 18) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();
    const uint16_t oem = static_cast<uint16_t>((buf[14] << 8) | buf[15]);
    const uint8_t key  = buf[16];
    const uint8_t sub  = buf[17];
    if (oem != 0xFFFF) return;  // not a global trigger
    if (key == 1) {             // KeyMacro: blackout 1 = toggle, 2 = on, 3 = off
        if (sub == 1) dmx::blackout_toggle();
        if (sub == 2) dmx::blackout_set(dmx::kAllOutputs, true);
        if (sub == 3) dmx::blackout_set(dmx::kAllOutputs, false);
        ESP_LOGI(TAG, "ArtTrigger: blackout macro %u", static_cast<unsigned>(sub));
        return;
    }
    if (key != 3) return;  // KeyShow drives the scenes
    if (sub == 0) {
        dmx::scene_stop();
        ESP_LOGI(TAG, "ArtTrigger: scene stop");
    } else if (sub <= config::num_scenes()) {
        dmx::scene_start(static_cast<uint8_t>(sub - 1));
        ESP_LOGI(TAG, "ArtTrigger: scene %u", static_cast<unsigned>(sub - 1));
    }
}

// ArtTimeCode — slaves FSEQ playback to the controller's clock. Only active
// while a file is playing (a timecode never auto-starts playback); small
// drift is tolerated so per-frame timecode doesn't thrash the pacing clock.
constexpr uint32_t kTimeCodeToleranceMs = 100;

void handle_time_code(const uint8_t* buf, size_t len) {
    parser::TimeCodeFields tc{};
    if (!parser::parse_time_code(buf, len, &tc)) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();
    if (fseq::status() != fseq::Status::Playing) return;
    const uint32_t target = parser::time_code_to_ms(tc);
    const uint32_t pos    = fseq::position_ms();
    const uint32_t drift  = target > pos ? target - pos : pos - target;
    if (drift > kTimeCodeToleranceMs) {
        fseq::seek_ms(target);
        ESP_LOGI(TAG, "ArtTimeCode: seek %u ms (drift %u ms)", static_cast<unsigned>(target),
                 static_cast<unsigned>(drift));
    }
}

// ArtCommand — validated + counted so controllers see it land (stats
// artnet_ctrl_rx); a consumer is future work per TODO.md.
void handle_counted_only(uint16_t op, const uint8_t* buf, size_t len) {
    if (op != parser::kOpCommand) return;
    if (len < 16) {
        dmx::note_packet_bad();
        return;
    }
    (void)buf;
    dmx::note_ctrl_rx();
}

void task_main(void*) {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(nullptr);
        return;
    }

    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(kArtnetPort);

    if (bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(g_sock);
        g_sock = -1;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "listening on UDP %d", kArtnetPort);

    // Static: 1500 B on this task's 4 kB stack left too little under the
    // log path (capture hook + two vfprintf) — an interrupt landing there
    // overflowed it. One artnet_rx task at a time, so no sharing.
    static uint8_t buf[1500];
    while (g_run) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        uint16_t op = 0;
        if (!parser::parse_header(buf, n, &op)) {
            dmx::note_packet_bad();
            continue;
        }
        switch (op) {
        case parser::kOpDmx: handle_dmx(buf, n, from); break;
        case parser::kOpPoll: handle_poll(buf, n, from); break;
        case parser::kOpSync: handle_sync(buf, n); break;
        case parser::kOpAddress: handle_address(buf, n, from); break;
        case parser::kOpIpProg: handle_ip_prog(buf, n, from); break;
        case parser::kOpNzs: handle_nzs(buf, n); break;
        case parser::kOpTrigger: handle_trigger(buf, n); break;
        case parser::kOpTimeCode: handle_time_code(buf, n); break;
        case parser::kOpCommand: handle_counted_only(op, buf, n); break;
        default: break;
        }
    }

    close(g_sock);
    g_sock = -1;
    vTaskDelete(nullptr);
}

}  // namespace

void start() {
    if (g_task) return;
    g_run = true;
    xTaskCreatePinnedToCore(task_main, "artnet_rx", 4096, nullptr, 10, &g_task, 0);
}

void stop() {
    g_run = false;
    if (g_sock >= 0) shutdown(g_sock, SHUT_RDWR);
    g_task = nullptr;
}

void set_local_ip(uint32_t host_order_ip) {
    g_local_ip = host_order_ip;
}

}  // namespace pixfrog::artnet
