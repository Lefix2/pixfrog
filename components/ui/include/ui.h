// ui — OLED SSD1306 + Adafruit seesaw rotary encoder + menu state machine.
//
// Architecture:
//   - `ui_task` time-polls the seesaw over I2C at ~30 Hz (the loop already
//     runs at that rate for the encoder-LED animation and display refresh),
//     dispatches events into the menu state machine, then redraws.
//   - The seesaw INT_N line is deliberately NOT used: at 30 Hz the worst-case
//     input latency is one tick (~33 ms, imperceptible on a rotary detent),
//     and skipping the interrupt machinery removes two extra I2C latch-clear
//     reads per poll. A 4-wire harness (VCC/GND/SDA/SCL) is sufficient.

#pragma once

#include <stdint.h>

namespace pixfrog::ui {

struct InitConfig {
    // I2C (always - encoder + OLED when OLED mode)
    int i2c_port;
    int i2c_sda_gpio;
    int i2c_scl_gpio;
    uint32_t i2c_freq_hz;
    uint8_t encoder_addr;
    // OLED only (ignored in TFT mode)
    uint8_t oled_addr;
    // TFT only (ignored in OLED mode)
    int spi_host;
    int spi_clk_gpio;
    int spi_mosi_gpio;
    int spi_cs_gpio;
    int tft_dc_gpio;
    int tft_rst_gpio;
    uint32_t spi_freq_hz;
    int tft_width;
    int tft_height;
    int tft_backlight_gpio;  // -1 = no backlight control (BL hard-wired on)
};

// Initialize I2C, display, seesaw. Spawns ui_task on core 0 prio 4.
// Must be called after config_store::init().
bool start(const InitConfig& cfg);

// ── Item 9: IP propagation ─────────────────────────────────────────────────
// Called from a network event handler when Ethernet acquires (or loses)
// an address. Pass 0 for "no link" (renders as "—" on HOME).
void set_ip(uint32_t host_order_ip);
uint32_t get_ip();

// ── Item B4: Ethernet link state ───────────────────────────────────────────
// Fed by ETH_EVENT CONNECTED/DISCONNECTED handlers. Distinct from get_ip()
// since the link can be UP while DHCP is still pending an address.
void set_link_up(bool up);
bool is_link_up();

// Coarse network state for the HOME status icon. Derived from link + IP +
// DHCP in main.cpp, but kept as an explicit enum so the UI can render a
// distinct icon per state (cable unplugged, DHCP acquiring, connected, error)
// without re-deriving the logic. set_link_up()/set_ip() stay for back-compat.
enum class NetState : uint8_t {
    Disconnected,  // no link (cable out)
    Acquiring,     // link up, DHCP lease pending
    Connected,     // link up, IP acquired
    Error,         // link up but no route / IP conflict / config failure
};
void set_net_state(NetState s);
NetState get_net_state();

// Per-channel activity is read directly from dmx::is_channel_active()
// in menu.cpp — no UI-side cache needed.

}  // namespace pixfrog::ui
