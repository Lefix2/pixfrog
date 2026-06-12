// FPP MultiSync remote — follows an FPP/xSchedule master over UDP 32320.
// Opt-in (GlobalConfig::fpp_remote): no socket is open while disabled.
//
// Master → remote semantics (only sequence sync is handled, media ignored):
//   START  : play the named .fseq from frame 0
//   OPEN   : preload only — ignored here (START/SYNC always follows)
//   STOP   : stop playback
//   SYNC   : periodic position broadcast; we re-seek only when local drift
//            exceeds kToleranceMs so normal pacing stays metronomic. A SYNC
//            for a file we are not playing hot-joins the running show.

#include "fpp_sync.h"
#include "fpp_sync_parser.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "fseq_player.h"

namespace pixfrog::fpp {

namespace {

constexpr const char* TAG = "FPPSYNC";

// Master SYNC packets arrive about once a second; one frame of jitter is
// normal, so only correct drift beyond a few frames.
constexpr uint32_t kToleranceMs = 100;

TaskHandle_t g_task = nullptr;
int g_sock          = -1;
bool g_run          = false;

void handle_sync(const parser::SyncFields& f) {
    if (f.file_type != parser::kFileSeq) return;  // media sync: not our job

    switch (f.action) {
    case parser::kSyncStart:
        if (f.filename[0]) {
            ESP_LOGI(TAG, "master start: %s", f.filename);
            fseq::start(f.filename);
        }
        break;

    case parser::kSyncOpen: break;  // preload hint only

    case parser::kSyncStop:
        ESP_LOGI(TAG, "master stop");
        fseq::stop();
        break;

    case parser::kSyncSync: {
        if (f.filename[0] == '\0') break;
        const char* active = fseq::active_file();
        if (!active || strcasecmp(active, f.filename) != 0) {
            // Hot-join: master is mid-show and we're not playing it yet.
            ESP_LOGI(TAG, "hot-join: %s", f.filename);
            if (!fseq::start(f.filename)) break;
        }
        const uint32_t target = static_cast<uint32_t>(f.seconds_elapsed * 1000.0f);
        const uint32_t pos    = fseq::position_ms();
        const uint32_t drift  = target > pos ? target - pos : pos - target;
        if (drift > kToleranceMs) {
            fseq::seek_ms(target);
            ESP_LOGI(TAG, "sync: seek %u ms (drift %u ms)", static_cast<unsigned>(target),
                     static_cast<unsigned>(drift));
        }
        break;
    }
    }
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

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(parser::kPort);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(g_sock);
        g_sock = -1;
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // FPP masters broadcast AND multicast sync; join the group so either
    // reaches us. A failed join is non-fatal (broadcast still works).
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(parser::kMulticastGroup);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
        ESP_LOGW(TAG, "multicast join %s failed", parser::kMulticastGroup);

    ESP_LOGI(TAG, "listening on UDP %d", parser::kPort);

    uint8_t buf[512];
    while (g_run) {
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        parser::SyncFields f{};
        if (parser::parse_sync(buf, static_cast<size_t>(n), &f)) handle_sync(f);
        // Other FPP packet types (ping, command, …) are silently ignored.
    }

    close(g_sock);
    g_sock = -1;
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void start() {
    if (g_task) return;
    g_run = true;
    xTaskCreatePinnedToCore(task_main, "fpp_sync", 4096, nullptr, 9, &g_task, 0);
}

void stop() {
    g_run = false;
    if (g_sock >= 0) shutdown(g_sock, SHUT_RDWR);
}

bool is_running() {
    return g_task != nullptr;
}

}  // namespace pixfrog::fpp
