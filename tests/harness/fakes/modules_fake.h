// Controllable stand-ins for the modules the console drives but the harness
// does not build for real (hardware or heavy dependencies).
#pragma once
#include <cstdint>
#include <string>
namespace fake {
struct Modules {
    bool fpp_running = false;
    bool web_running = false;
    int8_t cal_mode  = -1;
    uint32_t ip      = 0xC0A80232;
    bool link_up     = true;
    int fpp_starts   = 0;
    int web_starts   = 0;
    std::string fseq_started;  // last fseq::start() argument
    bool fseq_stopped       = false;
    bool fseq_loop          = false;  // last fseq::start() loop flag
    int playlist_starts     = 0;      // fseq::start_playlist() calls
    bool sd_mounted         = true;   // fseq::sd_state()
    uint32_t ui_loop_age_ms = 33;     // ui::loop_age_ms()
    uint32_t display_stalls = 0;      // ui::display_stalls()
    bool audio_ready        = true;   // audio::ready()
    int audio_tests         = 0;      // audio::start_test() calls that started
};
Modules& modules();
void reset_modules();
}  // namespace fake
