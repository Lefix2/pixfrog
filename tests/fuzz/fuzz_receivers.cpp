// The real Art-Net and sACN receive loops (artnet.cpp, sacn.cpp) through the
// datagram fabric, down to the decoded pixel buffer. First byte: bit 0 picks
// the port; the rest is the datagram. Several datagrams per input are split
// on the "\xFF\xFE\xFD" marker so sequences (sync, merge, priority) are reached.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "artnet.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "fakes/fseq_fake.h"
#include "sacn.h"
#include "sacn_parser.h"
#include "shim_control.h"

using namespace pixfrog;

namespace {

void init_once() {
    static bool done = false;
    if (done) return;
    done = true;
    shim::nvs_wipe();
    config::init();
    dmx::init();
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = ch < 2 ? led::Protocol::WS2815 : led::Protocol::Off;
        c.pixel_count    = 200;
        c.universe_start = static_cast<uint16_t>(1 + ch * 2);
        c.dmx_start      = 1;
        config::set_channel(ch, c);
        dmx::mark_channel_dirty(ch);
    }
    auto g         = config::get_global();
    g.sacn_enabled = true;
    config::set_global(g);
    // The full control mode on universe 1 (shared with output 1), so desk
    // bytes also reach the show-control evaluation.
    auto c     = config::default_control();
    c.enabled  = 1;
    c.universe = 1;
    c.address  = 400;
    config::control_apply_preset(c, config::ControlPreset::Full);
    config::set_control(c);
    dmx::mark_global_dirty();
    dmx::handle_pending_remaps();
    artnet::set_local_ip(0xC0A80232);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    init_once();
    if (size < 1) return 0;
    shim::net_reset();  // also drops the idle hooks: set them again
    shim::tasks_forget();
    shim::net_on_idle(artnet::kArtnetPort, [] { artnet::stop(); });
    shim::net_on_idle(sacn::parser::kSacnPort, [] { sacn::stop(); });
    const bool to_sacn          = data[0] & 1;
    const uint16_t port         = to_sacn ? sacn::parser::kSacnPort : artnet::kArtnetPort;
    static const uint8_t kSep[] = { 0xFF, 0xFE, 0xFD };
    const uint8_t* p            = data + 1;
    const uint8_t* end          = data + size;
    while (p <= end) {
        const uint8_t* cut = static_cast<const uint8_t*>(
            memmem(p, static_cast<size_t>(end - p), kSep, sizeof(kSep)));
        const uint8_t* stop = cut ? cut : end;
        shim::net_push(port, std::vector<uint8_t>(p, stop), 0xC0A80201 + (data[0] >> 1 & 3));
        if (!cut) break;
        p = cut + sizeof(kSep);
    }
    if (to_sacn) {
        sacn::start();
        shim::run_task("sacn_rx");
    } else {
        artnet::start();
        shim::run_task("artnet_rx");
    }
    shim::advance_ms(5);
    dmx::swap_universes();
    dmx::update_show_control();
    dmx::take_fseq_request();
    for (size_t ch = 0; ch < 2; ++ch)
        dmx::decode_pixels_for_channel(ch);
    return 0;
}
