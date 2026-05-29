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

namespace pixfrog::artnet {

namespace {

constexpr const char* TAG = "ARTNET";

TaskHandle_t g_task     = nullptr;
int          g_sock     = -1;
bool         g_run      = false;
uint32_t     g_local_ip = 0;

void handle_dmx(const uint8_t* buf, size_t len) {
    parser::DmxFields f{};
    if (!parser::parse_dmx(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }

    // Item 4: filter by configured net/subnet.
    const auto& g = config::get_global();
    if (!parser::universe_matches(f.universe, g.artnet_net, g.artnet_subnet)) {
        return;   // legitimate Art-Net for another node — silently drop
    }

    uint8_t* dst = dmx::universe_back_buffer_for(f.universe);
    if (!dst) return;

    const size_t copy = f.data_len > dmx::kUniverseSize ? dmx::kUniverseSize : f.data_len;
    std::memcpy(dst, f.data, copy);
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
    char    node_report[64];

    const auto& g = config::get_global();
    for (uint8_t bind = 1; bind <= 2; ++bind) {
        std::snprintf(node_report, sizeof(node_report),
                      "#0001 [%u] pixfrog OK", static_cast<unsigned>(bind));

        parser::PollReplyInputs in{};
        in.local_ip_host = g_local_ip;
        in.artnet_net    = g.artnet_net;
        in.artnet_subnet = g.artnet_subnet;
        in.short_name    = g.short_name;
        in.long_name     = g.long_name;
        in.node_report   = node_report;
        in.mac           = mac;
        in.bind_index    = bind;

        const uint8_t base_ch = (bind - 1) * 4;
        for (uint8_t p = 0; p < 4; ++p) {
            const size_t ch = base_ch + p;
            in.sw_out[p] = (ch < config::kNumChannels)
                ? static_cast<uint8_t>(config::get_channel(ch).universe_start & 0x0F)
                : 0;
        }

        parser::build_poll_reply(pkt, in);
        int n = sendto(g_sock, pkt, sizeof(pkt), 0,
                       reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (n < 0) {
            ESP_LOGW(TAG, "ArtPollReply bind %u sendto failed", bind);
            return;
        }
    }
}

void handle_poll(const uint8_t* /*buf*/, size_t len, const sockaddr_in& from) {
    if (len < 14) { dmx::note_packet_bad(); return; }
    // Item B1: choose broadcast (default) vs unicast back to the poller's IP.
    const auto& g = config::get_global();
    const uint32_t target = g.artnet_poll_reply_unicast
        ? from.sin_addr.s_addr            // already network-order
        : htonl(INADDR_BROADCAST);
    send_poll_reply(target);
}

void handle_sync(const uint8_t* /*buf*/, size_t /*len*/) {
    dmx::note_sync();
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
        socklen_t   fl  = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0,
                        reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        uint16_t op = 0;
        if (!parser::parse_header(buf, n, &op)) {
            dmx::note_packet_bad();
            continue;
        }
        switch (op) {
            case parser::kOpDmx:  handle_dmx (buf, n);       break;
            case parser::kOpPoll: handle_poll(buf, n, from); break;
            case parser::kOpSync: handle_sync(buf, n);       break;
            default:                                          break;
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
