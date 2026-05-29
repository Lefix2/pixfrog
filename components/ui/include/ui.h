// ui — OLED SSD1306 + Adafruit seesaw rotary encoder + menu state machine.
//
// Architecture:
//   - The seesaw signals events via an INT line (active LOW).
//   - A GPIO ISR gives a binary semaphore.
//   - `ui_task` takes the semaphore, polls seesaw over I2C, dispatches events
//     into the menu state machine, then redraws the OLED.
//   - When idle, `ui_task` blocks on the semaphore + a 1 s timer for HOME refresh.

#pragma once

#include <stdint.h>

namespace pixfrog::ui {

struct InitConfig {
    int      i2c_port;
    int      i2c_sda_gpio;
    int      i2c_scl_gpio;
    uint32_t i2c_freq_hz;
    int      encoder_int_gpio;
    uint8_t  oled_addr;
    uint8_t  encoder_addr;
};

// Initialize I2C, OLED, seesaw, GPIO ISR. Spawns ui_task on core 0 prio 4.
// Must be called after config_store::init().
bool start(const InitConfig& cfg);

// ── Item 9: IP propagation ─────────────────────────────────────────────────
// Called from a network event handler when Ethernet acquires (or loses)
// an address. Pass 0 for "no link" (renders as "—" on HOME).
void     set_ip(uint32_t host_order_ip);
uint32_t get_ip();

// ── Item B4: Ethernet link state ───────────────────────────────────────────
// Fed by ETH_EVENT CONNECTED/DISCONNECTED handlers. Distinct from get_ip()
// since the link can be UP while DHCP is still pending an address.
void set_link_up(bool up);
bool is_link_up();

// Per-channel activity is read directly from dmx::is_channel_active()
// in menu.cpp — no UI-side cache needed.

}  // namespace pixfrog::ui
