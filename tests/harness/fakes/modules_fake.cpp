#include "modules_fake.h"

#include <cstring>

#include "fpp_sync.h"
#include "fseq_fake.h"
#include "fseq_player.h"
#include "led_output.h"
#include "ui.h"
#include "web_config.h"

namespace fake {
Modules& modules() {
    static Modules m;
    return m;
}
void reset_modules() {
    modules() = Modules{};
}
}  // namespace fake

namespace pixfrog {
namespace fpp {
void start() {
    ::fake::modules().fpp_running = true;
    ++::fake::modules().fpp_starts;
}
void stop() {
    ::fake::modules().fpp_running = false;
}
bool is_running() {
    return ::fake::modules().fpp_running;
}
}  // namespace fpp
#ifndef PIXFROG_HARNESS_REAL_WEB  // harness_web links the real web_config
namespace web {
void start() {
    ::fake::modules().web_running = true;
    ++::fake::modules().web_starts;
}
void stop() {
    ::fake::modules().web_running = false;
}
bool is_running() {
    return ::fake::modules().web_running;
}
}  // namespace web
#endif
namespace output {
size_t fb_bytes() {
    return 65664;
}
void set_calibration_mode(int8_t id) {
    ::fake::modules().cal_mode = id;
}
int8_t get_calibration_mode() {
    return ::fake::modules().cal_mode;
}
DebugCounters get_debug_counters() {
    DebugCounters c{};
    c.trans_done = 1234;
    c.encode_us  = 1000;
    return c;
}
}  // namespace output
namespace ui {
uint32_t get_ip() {
    return ::fake::modules().ip;
}
bool is_link_up() {
    return ::fake::modules().link_up;
}
}  // namespace ui
namespace fseq {
bool start(const char* filename) {
    ::fake::modules().fseq_started = filename;
    return std::strcmp(filename, "missing.fseq") != 0;
}
void stop() {
    ::fake::modules().fseq_stopped = true;
}
const char* active_file() {
    return ::fake::modules().fseq_started.empty() ? nullptr
                                                  : ::fake::modules().fseq_started.c_str();
}
uint32_t duration_ms() {
    return 60'000;
}
const char* error_string() {
    return "";
}
SdState sd_state() {
    return SdState::Mounted;
}
size_t list_files(char names[][kMaxNameLen], size_t max) {
    const char* files[] = { "show.fseq", "loop.fseq" };
    size_t n            = 0;
    for (; n < 2 && n < max; ++n) {
        std::strncpy(names[n], files[n], kMaxNameLen - 1);
        names[n][kMaxNameLen - 1] = '\0';
    }
    return n;
}
}  // namespace fseq
}  // namespace pixfrog
