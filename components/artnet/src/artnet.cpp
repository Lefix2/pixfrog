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

namespace pixfrog::artnet {

namespace {

constexpr const char* TAG = "ARTNET";

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

    // Item 4: filter by configured net/subnet.
    const auto& g = config::get_global();
    if (!parser::universe_matches(f.universe, g.artnet_net, g.artnet_subnet)) {
        return;  // legitimate Art-Net for another node — silently drop
    }

    // 2-source merge keyed by sender IP; a third concurrent sender is dropped.
    if (!dmx::write_universe_from_source(f.universe, f.data, f.data_len, from.sin_addr.s_addr,
                                         dmx::kArtnetMergeTimeoutUs)) {
        return;
    }
    dmx::note_packet_rx();

    const int ch = dmx::channel_for_universe(f.universe);
    if (ch >= 0) dmx::note_channel_activity(static_cast<size_t>(ch));
}

void send_poll_reply(uint32_t target_addr_net_order) {
    if (g_sock < 0 || g_local_ip == 0) return;

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_ETH);

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = target_addr_net_order;
    dst.sin_port        = htons(kArtnetPort);

    uint8_t pkt[parser::kPollReplySize];
    char node_report[64];

    const auto& g = config::get_global();
    for (uint8_t bind = 1; bind <= 2; ++bind) {
        std::snprintf(node_report, sizeof(node_report), "#0001 [%u] pixfrog OK",
                      static_cast<unsigned>(bind));

        parser::PollReplyInputs in{};
        in.local_ip_host = g_local_ip;
        in.artnet_net    = g.artnet_net;
        in.artnet_subnet = g.artnet_subnet;
        in.short_name    = g.short_name;
        in.long_name     = g.long_name;
        in.node_report   = node_report;
        in.mac           = mac;
        in.bind_index    = bind;

        in.merge_ltp = g.merge_mode == config::kMergeLtp;

        const uint8_t base_ch = (bind - 1) * 4;
        for (uint8_t p = 0; p < 4; ++p) {
            const size_t ch     = base_ch + p;
            const bool mapped   = ch < config::kNumChannels;
            const bool disabled = mapped && led::is_off(config::get_channel(ch).protocol);
            in.sw_out[p]        = mapped
                                    ? static_cast<uint8_t>(config::get_channel(ch).universe_start & 0x0F)
                                    : 0;
            in.port_enabled[p]  = mapped && !disabled;
            in.port_merging[p]  = mapped && !disabled && dmx::is_channel_merging(ch);
        }

        parser::build_poll_reply(pkt, in);
        int n = sendto(g_sock, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (n < 0) {
            ESP_LOGW(TAG, "ArtPollReply bind %u sendto failed", bind);
            return;
        }
    }
}

void handle_poll(const uint8_t* /*buf*/, size_t len, const sockaddr_in& from) {
    if (len < 14) {
        dmx::note_packet_bad();
        return;
    }
    // Item B1: choose broadcast (default) vs unicast back to the poller's IP.
    const auto& g         = config::get_global();
    const uint32_t target = g.artnet_poll_reply_unicast
                              ? from.sin_addr.s_addr  // already network-order
                              : htonl(INADDR_BROADCAST);
    send_poll_reply(target);
}

void handle_sync(const uint8_t* /*buf*/, size_t /*len*/) {
    dmx::note_sync();
}

// ArtAddress — remote programming from the desk: names, net/subnet switch,
// per-port universe low nibble. Applied fields persist to NVS; always answer
// with an ArtPollReply to the sender so it sees the new state.
void handle_address(const uint8_t* buf, size_t len, const sockaddr_in& from) {
    parser::AddressFields f{};
    if (!parser::parse_address(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();

    config::GlobalConfig g = config::get_global();
    bool global_changed    = false;

    if (f.net_switch & 0x80) {
        g.artnet_net   = f.net_switch & 0x7F;
        global_changed = true;
    }
    if (f.sub_switch & 0x80) {
        g.artnet_subnet = f.sub_switch & 0x0F;
        global_changed  = true;
    }
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

    // SwOut programs the low nibble of a port's universe. BindIndex 0/1 →
    // channels 1-4, BindIndex 2 → channels 5-8 (mirrors our ArtPollReply).
    const uint8_t bind   = (f.bind_index <= 1) ? 1 : f.bind_index;
    const size_t base_ch = static_cast<size_t>(bind - 1) * 4;
    for (uint8_t p = 0; p < 4; ++p) {
        if (!(f.sw_out[p] & 0x80)) continue;
        const size_t ch = base_ch + p;
        if (ch >= config::kNumChannels) continue;
        auto c           = config::get_channel(ch);
        c.universe_start = static_cast<uint16_t>((c.universe_start & ~0x0Fu) |
                                                 (f.sw_out[p] & 0x0F));
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }

    // Merge commands. The spec scopes AcMerge* to one port; pixfrog applies a
    // single node-wide merge mode, so any port's command switches it globally.
    if (f.command == parser::kAcCancelMerge) {
        dmx::merge_cancel_all();
        ESP_LOGI(TAG, "ArtAddress: merge cancelled");
    } else if (parser::is_merge_ltp_command(f.command) || parser::is_merge_htp_command(f.command)) {
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

    config::GlobalConfig g = config::get_global();
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
        ESP_LOGI(TAG, "ArtIpProg applied (cmd 0x%02X) — reboot to take effect", f.command);
    }

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
// NOT routed: the universe pool stores start-code-0 levels only, and the
// DMX512 encoder emits SC=0 frames (alternate-SC interleaving is future work).
void handle_nzs(const uint8_t* buf, size_t len) {
    parser::NzsFields f{};
    if (!parser::parse_nzs(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();
}

// ArtTrigger — show control from the desk. Global packets (Oem 0xFFFF) with
// Key 3 (KeyShow) drive the standalone scenes: SubKey 0 stops, SubKey 1..8
// plays scene N-1. Other keys/Oems are counted but ignored.
void handle_trigger(const uint8_t* buf, size_t len) {
    if (len < 18) {
        dmx::note_packet_bad();
        return;
    }
    dmx::note_ctrl_rx();
    const uint16_t oem = static_cast<uint16_t>((buf[14] << 8) | buf[15]);
    const uint8_t key  = buf[16];
    const uint8_t sub  = buf[17];
    if (oem != 0xFFFF || key != 3) return;  // not a global KeyShow trigger
    if (sub == 0) {
        dmx::scene_stop();
        ESP_LOGI(TAG, "ArtTrigger: scene stop");
    } else if (sub <= config::kNumScenes) {
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

    uint8_t buf[1500];
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
