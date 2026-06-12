// Host stub for fseq_player.  The emulator has no SD card, so init() always
// reports no card, list_files() returns 0, and start()/stop() manipulate a
// fake in-memory state so the FSEQ menu FSM can be exercised.

#include "fseq_player.h"

#include <cstring>

namespace pixfrog::fseq {

namespace {
char g_active[kMaxNameLen] = {};
Status g_status            = Status::Idle;
}  // namespace

bool init(const InitConfig& /*cfg*/) {
    return false;  // no SD card in emulator
}

size_t list_files(char /*names*/[][kMaxNameLen], size_t /*max*/) {
    return 0;
}

bool start(const char* filename) {
    if (!filename || !filename[0]) return false;
    strncpy(g_active, filename, kMaxNameLen - 1);
    g_active[kMaxNameLen - 1] = '\0';
    g_status                  = Status::Playing;
    return true;
}

void stop() {
    g_active[0] = '\0';
    g_status    = Status::Idle;
}

const char* active_file() {
    return g_active[0] ? g_active : nullptr;
}

SdState sd_state() {
    return SdState::Absent;  // emulator has no SD card
}

Status status() {
    return g_status;
}

const char* error_string() {
    return "";
}

}  // namespace pixfrog::fseq
