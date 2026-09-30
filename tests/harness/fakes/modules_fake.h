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
    bool fseq_stopped = false;
};
Modules& modules();
void reset_modules();
}  // namespace fake
