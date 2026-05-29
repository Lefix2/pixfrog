#include "artnet.h"

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

// Art-Net packet header constants (cf. Art-Net 4 spec).
constexpr const char kArtnetId[8] = {'A','r','t','-','N','e','t', 0};

constexpr uint16_t kOpDmx        = 0x5000;
constexpr uint16_t kOpPoll       = 0x2000;
constexpr uint16_t kOpPollReply  = 0x2100;
constexpr uint16_t kOpSync       = 0x5200;

// ArtPollReply is exactly 239 bytes for Art-Net 4.
constexpr size_t   kPollReplySize = 239;

TaskHandle_t g_task     = nullptr;
int          g_sock     = -1;
bool         g_run      = false;
uint32_t     g_local_ip = 0;        // host-order; 0 = no link

bool header_ok(const uint8_t* buf, size_t len, uint16_t& opcode_out) {
    if (len < 12) return false;
    if (std::memcmp(buf, kArtnetId, 8) != 0) return false;
    opcode_out = static_cast<uint16_t>(buf[8]) | (static_cast<uint16_t>(buf[9]) << 8);
    return true;
}

void handle_dmx(const uint8_t* buf, size_t len) {
    if (len < 18) { dmx::note_packet_bad(); return; }
    const uint16_t sub_uni  = buf[14];                  // low byte of universe
    const uint16_t net      = buf[15] & 0x7F;           // high 7 bits
    const uint16_t universe = (net << 8) | sub_uni;
    const uint16_t data_len = (static_cast<uint16_t>(buf[16]) << 8) | buf[17];
    if (18u + data_len > len) { dmx::note_packet_bad(); return; }

    // Item 4: filter by configured Art-Net net + subnet.
    // The universe field decomposes as: bits 14..8 = net (7), 7..4 = sub (4), 3..0 = universe (4).
    // We silently drop packets outside our (net, subnet) instead of bumping
    // bad_packets — they're legitimate Art-Net traffic for other nodes.
    const auto& g = config::get_global();
    const uint8_t pkt_net = (universe >> 8) & 0x7F;
    const uint8_t pkt_sub = (universe >> 4) & 0x0F;
    if (pkt_net != g.artnet_net || pkt_sub != g.artnet_subnet) return;

    uint8_t* dst = dmx::universe_back_buffer_for(universe);
    if (!dst) { return; }   // universe not mapped to any channel — silently drop

    const size_t copy = data_len > dmx::kUniverseSize ? dmx::kUniverseSize : data_len;
    std::memcpy(dst, buf + 18, copy);
    dmx::note_packet_rx();

    // Per-channel HOME activity indicator (item 10 of previous round).
    const int ch = dmx::channel_for_universe(universe);
    if (ch >= 0) dmx::note_channel_activity(static_cast<size_t>(ch));
}

// ── ArtPollReply ────────────────────────────────────────────────────────────
//
// Format (Art-Net 4): fixed 239 bytes. Most fields are zero for a simple
// LED node. The interesting bits are: IP, Net, SubSwitch, NumPorts,
// PortTypes, GoodOutputA, SwOut, MAC, BindIp, BindIndex.
//
// Since we have 8 output ports but Art-Net limits 4 ports per reply, we
// emit one reply per "bind index": index 1 covers channels 0..3, index 2
// covers channels 4..7. Consoles aggregate both.

void write_ip(uint8_t* dst, uint32_t host_ip) {
    dst[0] = static_cast<uint8_t>((host_ip >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((host_ip >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((host_ip >>  8) & 0xFF);
    dst[3] = static_cast<uint8_t>( host_ip        & 0xFF);
}

void build_poll_reply(uint8_t pkt[kPollReplySize], uint8_t bind_index) {
    std::memset(pkt, 0, kPollReplySize);

    std::memcpy(pkt, kArtnetId, 8);
    pkt[8]  = 0x00;            // OpCode LE: 0x2100
    pkt[9]  = 0x21;

    write_ip(pkt + 10, g_local_ip);

    pkt[14] = 0x36;            // Port LSB
    pkt[15] = 0x19;            // Port MSB (0x1936 = 6454 LE: lo,hi)

    pkt[16] = 0x00;            // VersionH (firmware)
    pkt[17] = 0x01;            // VersionL — bump on releases

    const auto& g = config::get_global();
    pkt[18] = g.artnet_net & 0x7F;
    pkt[19] = g.artnet_subnet & 0x0F;

    pkt[20] = 0x00;            // OemHi
    pkt[21] = 0x00;            // OemLo (unknown OEM)
    pkt[22] = 0x00;            // UbeaVersion
    pkt[23] = 0xD0;            // Status1: bit6+7=indicator normal, bit5+4=PortAddr from frontpanel
    pkt[24] = 0xFF;            // EstaMan lo
    pkt[25] = 0xFF;            // EstaMan hi (0xFFFF = unknown)

    std::strncpy(reinterpret_cast<char*>(pkt + 26), g.short_name, 18);
    std::strncpy(reinterpret_cast<char*>(pkt + 44), g.long_name,  64);

    std::snprintf(reinterpret_cast<char*>(pkt + 108), 64,
                  "#0001 [%u] pixfrog OK", static_cast<unsigned>(bind_index));

    pkt[172] = 0x00;           // NumPortsHi
    pkt[173] = 0x04;           // NumPortsLo (always 4 max per reply)

    const uint8_t base_ch = (bind_index - 1) * 4;
    for (uint8_t p = 0; p < 4; ++p) {
        const size_t ch = base_ch + p;
        pkt[174 + p] = 0x80;   // PortType: output, DMX512
        pkt[182 + p] = 0x80;   // GoodOutputA: data being transmitted
        if (ch < config::kNumChannels) {
            const auto& cc = config::get_channel(ch);
            pkt[190 + p] = cc.universe_start & 0x0F;   // SwOut: low 4 bits of universe
        }
    }

    pkt[200] = 0x00;           // Style: StNode (output device)

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_ETH);
    std::memcpy(pkt + 201, mac, 6);

    write_ip(pkt + 207, g_local_ip);
    pkt[211] = bind_index;     // 1 = primary, increments for bind nodes
    pkt[212] = 0x08;           // Status2: bit3 = DHCP capable
}

void send_poll_reply() {
    if (g_sock < 0 || g_local_ip == 0) return;
    uint8_t pkt[kPollReplySize];
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = htonl(0xFFFFFFFFu);   // 255.255.255.255
    dst.sin_port        = htons(kArtnetPort);
    for (uint8_t bind = 1; bind <= 2; ++bind) {
        build_poll_reply(pkt, bind);
        int n = sendto(g_sock, pkt, sizeof(pkt), 0,
                       reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (n < 0) {
            ESP_LOGW(TAG, "ArtPollReply bind %u sendto failed", bind);
            return;
        }
    }
}

void handle_poll(const uint8_t* buf, size_t len, const sockaddr_in& /*from*/) {
    // Validate header (already done by caller) and length.
    if (len < 14) { dmx::note_packet_bad(); return; }
    // We ignore the TalkToMe and Priority bytes; just reply unconditionally.
    send_poll_reply();
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

    // Allow broadcast sends for ArtPollReply.
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
        if (!header_ok(buf, n, op)) { dmx::note_packet_bad(); continue; }
        switch (op) {
            case kOpDmx:  handle_dmx (buf, n);       break;
            case kOpPoll: handle_poll(buf, n, from); break;
            case kOpSync: handle_sync(buf, n);       break;
            default:      break;
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
