// main.cpp end to end: app_main() boots every real module against the shims
// (OLED UI build, menu faked), then the render task is driven frame by frame.
// An Art-Net packet in, its pixels decoded back off the PARLIO wire out; the
// Ethernet link events, static vs DHCP addressing, OTA rollback records and
// the pending-verify confirmation are checked along the way.

#include <cstring>
#include <vector>

#include "artnet.h"
#include "artnet_parser.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "esp32_p4_devkit.h"
#include "harness.h"
#include "led_output.h"
#include "net.h"
#include "shim_control.h"
#include "ui.h"

#include "esp_eth.h"
#include "esp_netif.h"
#include "lwip/inet.h"

extern "C" void app_main();

using namespace pixfrog;
using Bytes = std::vector<uint8_t>;

namespace {

constexpr uint16_t kArt = artnet::kArtnetPort;

Bytes art_dmx(uint16_t universe, const Bytes& data) {
    Bytes p(18 + data.size(), 0);
    std::memcpy(p.data(), "Art-Net\0", 8);
    p[8]  = static_cast<uint8_t>(artnet::parser::kOpDmx & 0xFF);
    p[9]  = static_cast<uint8_t>(artnet::parser::kOpDmx >> 8);
    p[11] = 14;
    p[14] = static_cast<uint8_t>(universe & 0xFF);
    p[15] = static_cast<uint8_t>(universe >> 8);
    p[16] = static_cast<uint8_t>(data.size() >> 8);
    p[17] = static_cast<uint8_t>(data.size() & 0xFF);
    std::memcpy(p.data() + 18, data.data(), data.size());
    return p;
}

// NRZ decode of bus bit `bit` (WS281x: '1' when the high pulse is long).
Bytes decode_nrz(const std::vector<uint16_t>& s, int bit) {
    Bytes out;
    uint8_t cur = 0;
    int nbits   = 0;
    for (size_t i = 0; i < s.size();) {
        if (!((s[i] >> bit) & 1)) {
            ++i;
            continue;
        }
        size_t hi = 0;
        for (; i < s.size() && ((s[i] >> bit) & 1); ++i)
            ++hi;
        cur = static_cast<uint8_t>((cur << 1) | (hi > 10 ? 1 : 0));
        if (++nbits == 8) {
            out.push_back(cur);
            cur   = 0;
            nbits = 0;
        }
    }
    return out;
}

// What the next app_main() boots with (persisted, as the device would have).
void configure(bool dhcp) {
    auto g           = config::get_global();
    g.use_dhcp       = dhcp;
    g.static_ip      = dhcp ? 0 : 0xC0A80232;  // 192.168.2.50
    g.static_mask    = 0;                      // unset: /24
    g.static_gateway = 0xC0A80201;
    g.web_enabled    = true;
    g.sacn_enabled   = true;
    g.fpp_remote     = true;
    g.boot_scene     = 0;
    config::set_global(g);
    for (size_t ch = 0; ch < config::kNumChannels; ++ch) {
        auto c           = config::get_channel(ch);
        c.protocol       = ch == 0 ? led::Protocol::WS2815 : led::Protocol::Off;
        c.pixel_count    = 2;
        c.universe_start = 1;
        c.dmx_start      = 1;
        c.color_order    = led::ColorOrder::GRB;
        c.brightness     = 255;
        c.gamma_x10      = 10;
        c.wb_r = c.wb_g = c.wb_b = 255;
        std::memset(c.gaps, 0, sizeof(c.gaps));
        config::set_channel(ch, c);
    }
}

void boot() {
    shim::tasks_forget();
    app_main();
}

void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        once = true;
    }
    shim::faults_clear();
    shim::boot_reset();
    shim::psram_present(true);
}

}  // namespace

TEST(boot_stops_short_without_universe_memory_or_led_output) {
    configure(false);
    shim::fail_next(shim::Fault::LdoAcquire);  // logged, boot goes on
    shim::fail_next(shim::Fault::HeapCaps);    // dmx::init's first bank
    boot();
    EXPECT_FALSE(shim::task_created("sd_mon"));  // stopped before the SD card
    EXPECT_FALSE(shim::task_created("render"));
    shim::psram_present(false);  // led_output refuses to start without PSRAM
    boot();
    EXPECT_TRUE(shim::task_created("sd_mon"));
    EXPECT_FALSE(shim::task_created("render"));
    EXPECT_FALSE(shim::netif_log().created);  // never reached the network
    net::apply(config::get_global());         // nothing to apply it to: a no-op
    EXPECT_FALSE(shim::netif_log().dhcp_stopped);
}

TEST(a_rejected_ota_image_is_recorded_once) {
    config::RollbackRecord rec{};
    shim::ota_invalid_partition("ota_1", "v9.9.9-bad", 7);
    shim::fail_next(shim::Fault::OtaDescription);  // unreadable: nothing recorded
    shim::fail_next(shim::Fault::HeapCaps);
    boot();
    EXPECT_FALSE(config::get_rollback(rec));
    shim::fail_next(shim::Fault::HeapCaps);
    boot();
    EXPECT_TRUE(config::get_rollback(rec));
    EXPECT_STREQ(rec.rejected_version, "v9.9.9-bad");
    EXPECT_STREQ(rec.rejected_slot, "ota_1");
    EXPECT_STREQ(rec.running_version, "v0.0.0-host");
    EXPECT_EQ(rec.rejected_sha[0], 7);
    // The same image on the next boot: already known, left as is.
    rec.acknowledged = true;
    config::set_rollback(rec);
    shim::fail_next(shim::Fault::HeapCaps);
    boot();
    EXPECT_TRUE(config::get_rollback(rec) && rec.acknowledged);
    // A new rejected image with a 32-char version (no NUL in the descriptor).
    shim::ota_invalid_partition("ota_0", "v1.2.3-45-gabcdef0123456789-dirt", 9);
    shim::fail_next(shim::Fault::HeapCaps);
    boot();
    EXPECT_TRUE(config::get_rollback(rec));
    EXPECT_FALSE(rec.acknowledged);
    EXPECT_EQ(std::strlen(rec.rejected_version), sizeof(rec.rejected_version) - 1);
}

TEST(the_first_full_boot_starts_everything_on_dhcp) {
    // The first boot to get past led_output in this process: the services it
    // starts stay started (later app_main() calls find them running).
    configure(true);
    boot();
    const auto& n = shim::netif_log();
    EXPECT_TRUE(n.eth_started);
    EXPECT_FALSE(n.dhcp_stopped);
    EXPECT_EQ(n.ldo_chan, 4);  // VDD_IO_5 pads at 3.3 V
    EXPECT_EQ(n.ldo_mv, 3300);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_START);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    EXPECT_TRUE(ui::is_link_up());
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Acquiring);
    EXPECT_EQ(ui::get_ip(), 0u);
    ip_event_got_ip_t got{};
    got.ip_info.ip.addr = lwip_htonl(0x0A000105);
    shim::event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got);
    EXPECT_EQ(ui::get_ip(), 0x0A000105u);
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Connected);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED);
    EXPECT_EQ(ui::get_ip(), 0u);
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Disconnected);
    shim::event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got);  // a late lease, cable out
    EXPECT_EQ(ui::get_ip(), 0u);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_STOP);
    shim::event_post(ETH_EVENT, 99);  // unknown: ignored
    for (const char* t :
         { "render", "compose", "artnet_rx", "sacn_rx", "fpp_sync" })  // sd_mon: above
        EXPECT_TRUE(shim::task_created(t));
    EXPECT_FALSE(shim::task_created("ota_confirm"));  // a confirmed image
}

namespace {
int g_frame = 0;
Bytes g_wire;
int8_t g_cal_seen = -1;
size_t g_pix_len  = 0;  // samples of a pixel frame / of a calibration frame
size_t g_cal_len  = 0;
void render_script() {
    ++g_frame;
    if (g_frame == 2) {
        g_wire    = decode_nrz(shim::parlio_log().last_samples, 0);
        g_pix_len = shim::parlio_log().last_samples.size();
    }
    if (g_frame == 4) g_cal_len = shim::parlio_log().last_samples.size();
    if (g_frame == 3) output::set_calibration_mode(0);  // scope pattern instead of pixels
    if (g_frame == 5) {
        g_cal_seen = output::get_calibration_mode();
        output::set_calibration_mode(-1);
    }
    if (g_frame == 10) shim::advance_ms(500);  // a stalled frame: resync, no burst
}
}  // namespace

TEST(the_render_task_puts_artnet_pixels_on_the_wire) {
    // Driven from the first full boot's tasks (see above).
    shim::net_on_idle(kArt, [] { artnet::stop(); });
    shim::net_push(kArt, art_dmx(1, { 0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC }));
    shim::run_task("artnet_rx");
    g_frame = 0;
    g_wire.clear();
    const int64_t t0 = shim::now_us();
    // The pipeline, one frame at a time: compose (core 0) publishes a frame and
    // composes the next, which waits for the encoder; render (core 1) encodes
    // the published one and frees the fronts. On one host thread the two take
    // turns — each watchdog kick is where the task gives the CPU back.
    for (int f = 0; f < 80; ++f) {
        EXPECT_TRUE(shim::run_task_for("compose", 2, nullptr, true));
        EXPECT_TRUE(shim::run_task_for("render", 1, render_script, true));
    }
    EXPECT_EQ(g_frame, 80);  // one encoded frame per turn: nothing lost, nothing doubled
    EXPECT_EQ(g_wire.size(), 6u);
    if (g_wire.size() == 6) {
        EXPECT_EQ(g_wire[0], 0x22);  // GRB
        EXPECT_EQ(g_wire[1], 0x11);
        EXPECT_EQ(g_wire[5], 0xCC);
    }
    EXPECT_EQ(g_cal_seen, 0);
    EXPECT_TRUE(g_cal_len >= 262144 && g_pix_len < 10000);  // the scope pattern went out
    EXPECT_EQ(shim::wdt_resets(), 3 * 80);
    // 80 frames at the configured rate span more than a second: FPS published,
    // and the stalled frame did not trigger a catch-up burst.
    const int64_t span_ms = (shim::now_us() - t0) / 1000;
    const uint32_t rate   = config::get_global().refresh_rate_hz;
    EXPECT_TRUE(dmx::get_stats().current_fps > 0);
    EXPECT_TRUE(span_ms >= static_cast<int64_t>(79 * 1000 / rate) + 400);
}

// Each side of the pipeline waits for the other: the encoder emits nothing
// that was not composed, and compose publishes nothing while the encoder
// still holds the fronts.
TEST(the_frame_pipeline_waits_for_its_partner) {
    const auto emitted = [] { return dmx::get_stats().frames_emitted; };
    const uint64_t e0  = emitted();
    EXPECT_TRUE(shim::run_task_for("render", 3, nullptr, true));  // nothing composed yet
    EXPECT_EQ(emitted(), e0);
    // Compose publishes one frame, then holds the next until it is encoded.
    EXPECT_TRUE(shim::run_task_for("compose", 6, nullptr, true));
    EXPECT_TRUE(shim::run_task_for("render", 3, nullptr, true));
    EXPECT_EQ(emitted(), e0 + 1);
    EXPECT_TRUE(shim::run_task_for("compose", 2, nullptr, true));
    EXPECT_TRUE(shim::run_task_for("render", 3, nullptr, true));
    EXPECT_EQ(emitted(), e0 + 2);
}

// No DHCP server: lwIP's AutoIP lands a 169.254 address (raised as GOT_IP).
// Link-local keeps it; Art-Net swaps it for 2.x/8 and asks DHCP again on the
// next link-up.
TEST(the_fallback_address_without_a_dhcp_server_is_configurable) {
    configure(true);
    auto g        = config::get_global();
    g.ip_fallback = config::kIpFallbackLinkLocal;
    config::set_global(g);
    boot();
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    ip_event_got_ip_t ll{};
    ll.ip_info.ip.addr = lwip_htonl(0xA9FE707A);  // 169.254.112.122
    shim::event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ll);
    EXPECT_EQ(ui::get_ip(), 0xA9FE707Au);
    EXPECT_FALSE(shim::netif_log().dhcp_stopped);  // DHCP keeps trying

    g.ip_fallback = config::kIpFallbackArtnet;  // read at the fallback, no reboot
    config::set_global(g);
    shim::event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ll);
    const uint32_t art = 0x02123456;  // 2.<mac 12 34 56>
    EXPECT_EQ(ui::get_ip(), art);
    const auto& n = shim::netif_log();
    EXPECT_TRUE(n.dhcp_stopped);
    EXPECT_EQ(n.ip, lwip_htonl(art));
    EXPECT_EQ(n.mask, lwip_htonl(0xFF000000));
    // A real lease is never replaced.
    ip_event_got_ip_t lease{};
    lease.ip_info.ip.addr = lwip_htonl(0x0A000105);
    shim::event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &lease);
    EXPECT_EQ(ui::get_ip(), 0x0A000105u);
    // Replug: DHCP is asked again (once).
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    EXPECT_EQ(n.dhcp_restarts, 1);
    EXPECT_FALSE(n.dhcp_stopped);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    EXPECT_EQ(n.dhcp_restarts, 1);
    g.ip_fallback = config::kIpFallbackLinkLocal;
    config::set_global(g);
}

TEST(static_mode_publishes_its_address_on_every_link_up) {
    configure(false);
    boot();
    const auto& n = shim::netif_log();
    EXPECT_TRUE(n.dhcp_stopped);
    EXPECT_EQ(n.ip, lwip_htonl(0xC0A80232));
    EXPECT_EQ(n.mask, lwip_htonl(0xFFFFFF00));  // unset mask → /24
    EXPECT_EQ(n.gw, lwip_htonl(0xC0A80201));
    EXPECT_EQ(n.handlers, 2);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED);
    EXPECT_EQ(ui::get_ip(), 0u);  // a static IP is not an address with the cable out
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    EXPECT_EQ(ui::get_ip(), 0xC0A80232u);
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Connected);
}

// A network setting changed from any surface re-addresses the box at once:
// to a static address the link makes usable on the spot, or back to DHCP,
// a fresh lease asked for and Acquiring until it comes.
TEST(the_addressing_is_applied_live) {
    configure(true);
    boot();
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    ip_event_got_ip_t got{};
    got.ip_info.ip.addr = lwip_htonl(0x0A000105);
    shim::event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got);
    EXPECT_EQ(ui::get_ip(), 0x0A000105u);
    EXPECT_TRUE(net::link_up());

    auto g           = config::get_global();
    g.use_dhcp       = false;
    g.static_ip      = 0xC0A801C8;  // 192.168.1.200
    g.static_mask    = 0xFFFFFF00;
    g.static_gateway = 0xC0A80101;
    net::apply(g);
    const auto& n = shim::netif_log();
    EXPECT_TRUE(n.dhcp_stopped);
    EXPECT_EQ(n.ip, lwip_htonl(0xC0A801C8));
    EXPECT_EQ(n.gw, lwip_htonl(0xC0A80101));
    EXPECT_EQ(ui::get_ip(), 0xC0A801C8u);  // usable at once: the link is up
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Connected);
    // The lease that was still coming is no business of the static mode.
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED);
    EXPECT_EQ(ui::get_ip(), 0u);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    EXPECT_EQ(ui::get_ip(), 0xC0A801C8u);

    const int restarts = n.dhcp_restarts;
    g.use_dhcp         = true;
    net::apply(g);
    EXPECT_EQ(n.dhcp_restarts, restarts + 1);
    EXPECT_FALSE(n.dhcp_stopped);
    EXPECT_EQ(ui::get_ip(), 0u);  // nothing to advertise until the server answers
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Acquiring);
    got.ip_info.ip.addr = lwip_htonl(0x0A000107);
    shim::event_post(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got);
    EXPECT_EQ(ui::get_ip(), 0x0A000107u);
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Connected);

    // A static address set with the cable out waits for the link.
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED);
    g.use_dhcp  = false;
    g.static_ip = 0xC0A801C9;
    net::apply(g);
    EXPECT_EQ(n.ip, lwip_htonl(0xC0A801C9));
    EXPECT_EQ(ui::get_ip(), 0u);
    EXPECT_TRUE(ui::get_net_state() == ui::NetState::Disconnected);
    shim::event_post(ETH_EVENT, ETHERNET_EVENT_CONNECTED);
    EXPECT_EQ(ui::get_ip(), 0xC0A801C9u);
    // Static with no address at all is DHCP.
    g.static_ip = 0;
    net::apply(g);
    EXPECT_EQ(n.dhcp_restarts, restarts + 2);
}

TEST(network_bring_up_failures_never_stop_the_boot) {
    configure(false);
    const shim::Fault faults[] = { shim::Fault::NetifNew, shim::Fault::EthMac, shim::Fault::EthPhy,
                                   shim::Fault::EthInstall, shim::Fault::EthStart };
    for (shim::Fault f : faults) {
        shim::boot_reset();
        shim::fail_next(f);
        boot();
        EXPECT_TRUE(shim::task_created("render"));  // the UI still lets the user fix it
        EXPECT_FALSE(shim::netif_log().eth_started);
    }
}

TEST(a_pending_image_is_confirmed_only_by_a_live_render_loop) {
    configure(false);
    shim::ota_pending_verify(true);
    boot();
    EXPECT_TRUE(shim::task_created("ota_confirm"));
    dmx::set_current_fps(44);
    shim::run_task("ota_confirm");
    EXPECT_TRUE(shim::ota_marked_valid());
    // Never rendering: rebooted after a minute so the bootloader rolls back.
    shim::boot_reset();
    shim::ota_pending_verify(true);
    boot();
    dmx::set_current_fps(0);
    const int restarts = shim::restarts();
    const int64_t t0   = shim::now_us();
    shim::run_task("ota_confirm");
    EXPECT_FALSE(shim::ota_marked_valid());
    EXPECT_EQ(shim::restarts(), restarts + 1);
    EXPECT_TRUE(shim::now_us() - t0 >= 60'000'000);
}

// GPIO53 pulled up at boot: high = R52 removed, the speaker can be driven;
// low = stock board (R52 into R58's 10 kΩ), the amp follows LED CH8: no audio.
TEST(the_speaker_is_only_driven_on_a_board_with_the_r52_mod) {
    configure(true);
    shim::i2s_reset();
    shim::gpio_input(board::kAmpProbeGpio, 0);
    boot();
    EXPECT_EQ(shim::i2s_log().channels, 0);  // stock: audio not even tried
    shim::gpio_input(board::kAmpProbeGpio, 1);
    boot();
    EXPECT_EQ(shim::i2s_log().channels, 1);  // modded: tried (no codec here: given back)
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}
