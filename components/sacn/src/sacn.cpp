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

#include <atomic>
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

// Receiver lifecycle, shared by the task and start()/stop() (other tasks):
// a stop() only asks; the task leaves by moving Stopping → Off itself, so a
// start() landing meanwhile turns Stopping back into Running and the same
// task carries on. 32-bit: the P4 only does word-sized atomic RMW.
enum : uint32_t { kOff, kRunning, kStopping };
std::atomic<uint32_t> g_state{ kOff };
std::atomic<uint32_t> g_sync_address{ 0 };  // the E1.31 sync address the data waits on, 0 = none
int g_sock = -1;

bool running() {
    return g_state.load(std::memory_order_acquire) == kRunning;
}

uint16_t g_joined[kMaxJoined];
size_t g_joined_count = 0;

void (*g_overflow_hook)(bool) = nullptr;
bool g_overflow               = false;

// Tells the board when the joined groups outgrow the MAC's filter table.
void update_overflow() {
    const bool over = g_joined_count > kHwMulticastSlots;
    if (over == g_overflow || !g_overflow_hook) return;
    g_overflow = over;
    g_overflow_hook(over);
    ESP_LOGI(TAG, "%u groups: MAC %s", static_cast<unsigned>(g_joined_count),
             over ? "passes all multicast" : "back to its address filter");
}

// Two sources per universe on average: each (universe, CID) pair is an entry.
parser::SourceGate g_gates[dmx::kNumUniverses * 2];

uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Universes the current config maps, deduplicated, capped to the pool size.
size_t wanted_universes(uint16_t out[kMaxJoined]) {
    size_t n = 0;
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        const auto& cc = config::get_channel(ch);
        if (led::is_off(cc.protocol)) continue;
        const size_t used = dmx::channel_universe_span(cc);  // packing + dmx_start
        for (size_t u = 0; u < used && n < kMaxJoined; ++u) {
            const uint32_t uni = static_cast<uint32_t>(cc.universe_start) + u;
            if (uni < 1 || !dmx::universe_routable(uni)) continue;
            bool dup = false;
            for (size_t i = 0; i < n; ++i)
                if (out[i] == uni) dup = true;
            if (!dup) out[n++] = static_cast<uint16_t>(uni);
        }
    }
    // The DMX control universe, when enabled.
    const int ctrl = dmx::control_universe();
    if (ctrl >= 1 && n < kMaxJoined) {
        bool dup = false;
        for (size_t i = 0; i < n; ++i)
            if (out[i] == ctrl) dup = true;
        if (!dup) out[n++] = static_cast<uint16_t>(ctrl);
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
    update_overflow();
}

void handle_data(const uint8_t* buf, size_t len) {
    parser::DataFields f{};
    if (!parser::parse_data(buf, len, &f)) {
        dmx::note_packet_bad();
        return;
    }
    if (f.options & parser::kOptPreview) return;  // visualizer-only data
    // E1.31 allows universes up to 63999; the pool is keyed on the 15-bit
    // Art-Net Port-Address and can never map anything above that. Drop those
    // here, before the number reaches any lookup. Not a malformed packet —
    // just one addressed to a universe this node cannot own.
    if (!dmx::universe_routable(f.universe)) return;

    const uint32_t source_id = parser::source_id_from_cid(f.cid);
    const bool terminated    = (f.options & parser::kOptTerminated) != 0;
    if (terminated) {
        dmx::note_universe_terminated(f.universe);  // immediate failsafe (E1.31)
        dmx::merge_drop_source(f.universe, source_id);
    }
    bool takeover = false;
    if (!parser::gate_accept(g_gates, f.universe, source_id, f.priority, f.sequence, terminated,
                             now_ms(), &takeover))
        return;
    // Priority rose: the outranked sources' staging must not blend into HTP.
    if (takeover) dmx::merge_reset_universe(f.universe);
    if (f.start_code != 0) return;  // alternate start codes: not routed (cf. ArtNzs)

    // Equal-priority sources land here together; the 2-source merge (keyed by
    // CID hash) arbitrates HTP/LTP exactly like concurrent ArtDmx senders.
    if (!dmx::write_universe_from_source(f.universe, f.data, f.data_len, source_id,
                                         dmx::kSacnMergeTimeoutUs)) {
        return;  // unmapped universe, or a third concurrent source
    }
    dmx::note_sacn_rx();

    dmx::note_universe_activity(f.universe);  // every output it feeds

    // E1.31 §6.2.4: data carrying a synchronization address waits for a sync
    // packet on that address; Force_Synchronization keeps waiting if syncs stop.
    if (f.sync_address) {
        g_sync_address.store(f.sync_address, std::memory_order_relaxed);
        dmx::note_sync_hold(dmx::kE131SyncTimeoutMs, (f.options & parser::kOptForceSync) != 0);
    } else {
        dmx::note_sync_released();
    }
}

void task_main(void*) {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        g_state.store(kOff, std::memory_order_release);
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
        g_state.store(kOff, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    refresh_memberships();
    uint32_t last_refresh = now_ms();
    ESP_LOGI(TAG, "listening on UDP %d, %u multicast groups joined", parser::kSacnPort,
             static_cast<unsigned>(g_joined_count));

    static uint8_t buf[700];  // E1.31 data packet max = 638 bytes; off the 4 kB stack
    for (;;) {
        while (running()) {
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            int n = recvfrom(g_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);

            if (now_ms() - last_refresh >= kMembershipRefreshMs) {
                refresh_memberships();
                last_refresh = now_ms();
            }
            if (n <= 0 || !running()) continue;  // disabled: data no longer lands

            const uint32_t root = parser::parse_root(buf, n);
            if (root == parser::kRootVectorData) {
                handle_data(buf, n);
            } else if (root == parser::kRootVectorExtended) {
                // Only the sync address the data asked for releases it.
                uint16_t sync_addr   = 0;
                const uint32_t asked = g_sync_address.load(std::memory_order_relaxed);
                if (!parser::parse_sync(buf, n, &sync_addr))
                    dmx::note_packet_bad();
                else if (!asked || sync_addr == asked)
                    dmx::note_sync(dmx::kE131SyncTimeoutMs);
            } else {
                dmx::note_packet_bad();
            }
        }
        uint32_t expected = kStopping;
        if (g_state.compare_exchange_strong(expected, kOff, std::memory_order_acq_rel)) break;
        // Re-enabled before we left: keep serving with the same socket.
    }

    for (size_t i = 0; i < g_joined_count; ++i)
        membership(g_joined[i], IP_DROP_MEMBERSHIP);
    g_joined_count = 0;
    update_overflow();
    close(g_sock);
    g_sock = -1;
    vTaskDelete(nullptr);
}

}  // namespace

void start() {
    uint32_t s = g_state.load(std::memory_order_acquire);
    for (;;) {
        if (s == kRunning) return;
        if (s == kStopping) {
            // Cancel the pending stop: the live task keeps serving.
            if (g_state.compare_exchange_weak(s, kRunning, std::memory_order_acq_rel)) return;
            continue;
        }
        if (g_state.compare_exchange_weak(s, kRunning, std::memory_order_acq_rel)) break;
    }
    if (xTaskCreatePinnedToCore(task_main, "sacn_rx", 4096, nullptr, 10, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        g_state.store(kOff, std::memory_order_release);
    }
}

void set_multicast_overflow_hook(void (*hook)(bool pass_all)) {
    g_overflow_hook = hook;
}

void stop() {
    uint32_t expected = kRunning;
    g_state.compare_exchange_strong(expected, kStopping, std::memory_order_acq_rel);
}

bool is_running() {
    return running();
}

}  // namespace pixfrog::sacn
