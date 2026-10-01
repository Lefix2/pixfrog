// Host stub for fseq_player. No SD card by default; `set sd <n>` on the agent
// API fakes one holding n files. start()/stop() manipulate an in-memory state
// so the FSEQ menu FSM can be exercised.

#include "fseq_player.h"

#include <cstdio>
#include <cstring>

#include "dmx_emu.h"

namespace pixfrog::fseq {

namespace {
char g_active[kMaxNameLen] = {};
Status g_status            = Status::Idle;
int g_files                = 0;
}  // namespace

bool init(const InitConfig& /*cfg*/) {
    return false;  // no SD card in emulator
}

size_t list_files(char names[][kMaxNameLen], size_t max) {
    size_t n = 0;
    for (; n < static_cast<size_t>(g_files) && n < max; ++n)
        std::snprintf(names[n], kMaxNameLen, "show%u.fseq", static_cast<unsigned>(n + 1));
    return n;
}

bool start(const char* filename, bool /*loop*/) {
    if (!filename || !filename[0]) return false;
    strncpy(g_active, filename, kMaxNameLen - 1);
    g_active[kMaxNameLen - 1] = '\0';
    g_status                  = Status::Playing;
    return true;
}

bool start_playlist() {
    return false;  // no SD card in the emulator
}

bool looping() {
    return false;
}

int playlist_index() {
    return -1;
}

void stop() {
    g_active[0] = '\0';
    g_status    = Status::Idle;
}

const char* active_file() {
    return g_active[0] ? g_active : nullptr;
}

SdState sd_state() {
    return g_files ? SdState::Mounted : SdState::Absent;
}

Status status() {
    return g_status;
}

const char* error_string() {
    return "";
}

void emu_set_files(int n) {
    g_files = n < 0 ? 0 : n;
}

}  // namespace pixfrog::fseq

void emu_fseq_set_files(int n) {
    pixfrog::fseq::emu_set_files(n);
}
