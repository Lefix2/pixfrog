#include "artnet.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "config_store.h"
#include "dmx_manager.h"

namespace pixfrog::artnet {

namespace {

constexpr const char* TAG = "ARTNET";

// Art-Net packet header constants (cf. Art-Net 4 spec).
constexpr const char kArtnetId[8] = {'A','r','t','-','N','e','t', 0};

constexpr uint16_t kOpDmx        = 0x5000;
constexpr uint16_t kOpPoll       = 0x2000;
constexpr uint16_t kOpPollReply  = 0x2100;
constexpr uint16_t kOpSync       = 0x5200;

TaskHandle_t g_task = nullptr;
int          g_sock = -1;
bool         g_run  = false;

bool header_ok(const uint8_t* buf, size_t len, uint16_t& opcode_out) {
    if (len < 12) return false;
    if (std::memcmp(buf, kArtnetId, 8) != 0) return false;
    // little-endian opcode at bytes 8..9
    opcode_out = static_cast<uint16_t>(buf[8]) | (static_cast<uint16_t>(buf[9]) << 8);
    return true;
}

void handle_dmx(const uint8_t* buf, size_t len) {
    if (len < 18) { dmx::note_packet_bad(); return; }
    // sequence at buf[12], physical at buf[13]
    const uint16_t sub_uni = buf[14];                // low byte of universe
    const uint16_t net     = buf[15] & 0x7F;         // high 7 bits
    const uint16_t universe = (net << 8) | sub_uni;  // combined 15-bit
    const uint16_t data_len = (static_cast<uint16_t>(buf[16]) << 8) | buf[17];
    if (18u + data_len > len) { dmx::note_packet_bad(); return; }

    uint8_t* dst = dmx::universe_back_buffer_for(universe);
    if (!dst) { /* universe not mapped to a channel — silently drop */ return; }

    const size_t copy = data_len > dmx::kUniverseSize ? dmx::kUniverseSize : data_len;
    std::memcpy(dst, buf + 18, copy);
    dmx::note_packet_rx();
}

void handle_poll(const uint8_t* /*buf*/, size_t /*len*/, const sockaddr_in& /*from*/) {
    // TODO(v1): emit ArtPollReply.
    // Skipped in v0 — most consoles still discover us via static IP entry.
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
        if (!header_ok(buf, n, op)) { dmx::note_packet_bad(); continue; }
        switch (op) {
            case kOpDmx:  handle_dmx (buf, n);        break;
            case kOpPoll: handle_poll(buf, n, from);  break;
            case kOpSync: handle_sync(buf, n);        break;
            default:      /* unsupported — silently drop */ break;
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

}  // namespace pixfrog::artnet
