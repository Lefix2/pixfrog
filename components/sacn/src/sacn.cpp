// sACN / E1.31 receiver — feeds the same dmx_manager universe pool as the
// ArtNet receiver. Opt-in (GlobalConfig::sacn_enabled): no socket is open
// while disabled.
//
// Universe numbering: the sACN universe is matched against the channels'
// flat `universe_start` numbers, exactly like the ArtNet port-address. No
// net/subnet filter applies (that concept is ArtNet-only).
//
// Multicast: one IGMP join per configured universe (239.255.hi.lo). The
// wanted set is recomputed every kMembershipRefreshMs from the live config,
// so UI/console/web edits are picked up without an explicit notification.
// Unicast sACN is accepted at any time.

#include "sacn.h"
#include "sacn_parser.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "config_store.h"
#include "dmx_manager.h"
#include "led_protocols.h"

namespace pixfrog::sacn {

namespace {

constexpr const char* TAG = "SACN";

constexpr uint32_t kMembershipRefreshMs = 5000;
constexpr size_t kMaxJoined             = dmx::kNumUniverses;

TaskHandle_t g_task = nullptr;
int g_sock          = -1;
bool g_run          = false;

uint16_t g_joined[kMaxJoined];
size_t g_joined_count = 0;

parser::SourceGate g_gates[dmx::kNumUniverses];

uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Universes the current config maps, deduplicated, capped to the pool size.
size_t wanted_universes(uint16_t out[kMaxJoined]) {
    size_t n = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& cc = config::get_channel(ch);
        if (led::is_off(cc.protocol)) continue;
        const size_t total = static_cast<size_t>(cc.pixel_count) *
                             led::bytes_per_pixel(cc.protocol);
        const size_t used = (total + dmx::kUniverseSize - 1) / dmx::kUniverseSize;
        for (size_t u = 0; u < used && n < kMaxJoined; ++u) {
            const uint32_t uni = static_cast<uint32_t>(cc.universe_start) + u;
            if (uni < 1 || uni > 63999) continue;
            bool dup = false;
            for (size_t i = 0; i < n; ++i)
                if (out[i] == uni) dup = true;
            if (!dup) out[n++] = static_cast<uint16_t>(uni);
        }
    }
    return n;
}

bool membership(uint16_t universe, int op) {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = htonl(parser::multicast_group_host(universe));
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(g_sock, IPPROTO_IP, op, &mreq, sizeof(mreq)) == 0;
}

void refresh_memberships() {
    uint16_t wanted[kMaxJoined];
    const size_t wn = wanted_universes(wanted);

    // Leave stale groups.
    for (size_t i = 0; i < g_joined_count;) {
        bool still = false;
        for (size_t j = 0; j < wn; ++j)
            if (wanted[j] == g_joined[i]) still = true;
        if (!still) {
            membership(g_joined[i], IP_DROP_MEMBERSHIP);
            g_joined[i] = g_joined[--g_joined_count];
        } else {
            ++i;
        }
    }
    // Join new ones.
    for (size_t j = 0; j < wn; ++j) {
        bool have = false;
        for (size_t i = 0; i < g_joined_count; ++i)
            if (g_joined[i] == wanted[j]) have = true;
        if (!have && g_joined_count < kMaxJoined) {
            if (membership(wanted[j], IP_ADD_MEMBERSHIP)) {
                g_joined[g_joined_count++] = wanted[j];
            } else {
                ESP_LOGW(TAG, "IGMP join universe %u failed", wanted[j]);
            }
        }
    }
}

void handle_data(const uint8_t* buf, size_t len) {
    parser::DataFields f{};
    if (!parser::parse_data(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }
    if (f.options & parser::kOptPreview) return;  // visualizer-only data

    const bool terminated = (f.options & parser::kOptTerminated) != 0;
    if (!parser::gate_accept(g_gates, f.universe, f.priority, terminated, now_ms())) return;
    if (f.start_code != 0) return;  // alternate start codes: not routed (cf. ArtNzs)

    uint8_t* dst = dmx::universe_back_buffer_for(f.universe);
    if (!dst) return;  // legitimate sACN for an unmapped universe

    const size_t copy = f.data_len > dmx::kUniverseSize ? dmx::kUniverseSize : f.data_len;
    std::memcpy(dst, f.data, copy);
    dmx::note_sacn_rx();

    const int ch = dmx::channel_for_universe(f.universe);
    if (ch >= 0) dmx::note_channel_activity(static_cast<size_t>(ch));

    if (f.options & parser::kOptForceSync) dmx::note_sync();
}

void task_main(void*) {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    timeval tv{};
    tv.tv_sec = 1;  // recv timeout paces the membership refresh + stop check
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(parser::kSacnPort);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(g_sock);
        g_sock = -1;
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    refresh_memberships();
    uint32_t last_refresh = now_ms();
    ESP_LOGI(TAG, "listening on UDP %d, %u multicast groups joined", parser::kSacnPort,
             static_cast<unsigned>(g_joined_count));

    uint8_t buf[700];  // E1.31 data packet max = 638 bytes
    while (g_run) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);

        if (now_ms() - last_refresh >= kMembershipRefreshMs) {
            refresh_memberships();
            last_refresh = now_ms();
        }
        if (n <= 0) continue;

        const uint32_t root = parser::parse_root(buf, n);
        if (root == parser::kRootVectorData) {
            handle_data(buf, n);
        } else if (root == parser::kRootVectorExtended) {
            uint16_t sync_addr = 0;
            if (parser::parse_sync(buf, n, &sync_addr)) dmx::note_sync();
        } else {
            dmx::note_packet_bad();
        }
    }

    for (size_t i = 0; i < g_joined_count; ++i)
        membership(g_joined[i], IP_DROP_MEMBERSHIP);
    g_joined_count = 0;
    close(g_sock);
    g_sock = -1;
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void start() {
    if (g_task) return;
    g_run = true;
    xTaskCreatePinnedToCore(task_main, "sacn_rx", 4096, nullptr, 10, &g_task, 0);
}

void stop() {
    g_run = false;
    if (g_sock >= 0) shutdown(g_sock, SHUT_RDWR);
}

bool is_running() {
    return g_task != nullptr;
}

}  // namespace pixfrog::sacn
