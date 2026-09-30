// Controllable stand-in for fseq_player (SD card + zstd playback are not part
// of the network-receiver harness). Tests set the state and read the seeks.
#include "fseq_fake.h"

namespace pixfrog::fseq {
namespace {
Status g_status   = Status::Idle;
uint32_t g_pos_ms = 0;
}  // namespace
Status status() {
    return g_status;
}
uint32_t position_ms() {
    return g_pos_ms;
}
bool seek_ms(uint32_t ms) {
    fake::seeks().push_back(ms);
    g_pos_ms = ms;
    return true;
}
namespace fake {
void set(Status s, uint32_t pos_ms) {
    g_status = s;
    g_pos_ms = pos_ms;
    seeks().clear();
}
std::vector<uint32_t>& seeks() {
    static std::vector<uint32_t> v;
    return v;
}
}  // namespace fake
}  // namespace pixfrog::fseq
