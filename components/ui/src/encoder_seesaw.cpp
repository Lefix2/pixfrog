// Adafruit seesaw 4991 (I2C rotary encoder) — minimal client.
//
// Two seesaw registers are relevant:
//   ENCODER_POSITION (4 bytes, signed) — read & diff to detect rotation
//   GPIO_PIN_24 (button on default board) — read to detect press
//
// We poll the seesaw on each INT-driven wake and emit Events.

#include "ui_internal.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

namespace pixfrog::ui::detail {

namespace {

constexpr const char* TAG = "ENC";

i2c_master_dev_handle_t g_dev      = nullptr;
int                     g_int_gpio = -1;
int32_t                 g_last_pos = 0;
bool                    g_last_btn = false;

// Pending events queue (simple ring) so we can drain multiple rotations
// per wake without losing them.
constexpr size_t kEvtQ = 8;
Event    g_q[kEvtQ];
size_t   g_qh = 0, g_qt = 0;

void push_evt(Event e) {
    g_q[g_qh] = e;
    g_qh = (g_qh + 1) % kEvtQ;
    if (g_qh == g_qt) g_qt = (g_qt + 1) % kEvtQ;  // drop oldest on overflow
}
Event pop_evt() {
    if (g_qh == g_qt) return Event::None;
    Event e = g_q[g_qt];
    g_qt = (g_qt + 1) % kEvtQ;
    return e;
}

bool seesaw_read_position(int32_t& /*pos_out*/) {
    // TODO: I2C write reg(0x11, 0x30) then read 4 bytes; combine BE.
    return false;
}
bool seesaw_read_button(bool& /*pressed_out*/) {
    // TODO: I2C write reg(0x01, 0x04) then read 4 bytes; bit24.
    return false;
}

}  // namespace

bool encoder_init(i2c_master_bus_handle_t bus, uint8_t addr, int int_gpio) {
    if (!bus) return false;
    g_int_gpio = int_gpio;
    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr;
    dev_cfg.scl_speed_hz    = 400'000;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &g_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus_add_device: %s", esp_err_to_name(err));
        return false;
    }
    // TODO: set seesaw INT enable on encoder & button registers via i2c_master_transmit.
    ESP_LOGI(TAG, "seesaw bus add OK at 0x%02X (register init still TODO)", addr);
    return true;
}

Event encoder_poll() {
    // Drain queued events first.
    Event e = pop_evt();
    if (e != Event::None) return e;

    int32_t pos = g_last_pos;
    bool    btn = g_last_btn;
    if (seesaw_read_position(pos)) {
        const int32_t delta = pos - g_last_pos;
        g_last_pos = pos;
        for (int i = 0; i < delta;  ++i) push_evt(Event::RotateRight);
        for (int i = 0; i < -delta; ++i) push_evt(Event::RotateLeft);
    }
    if (seesaw_read_button(btn)) {
        if (btn && !g_last_btn) push_evt(Event::Click);
        g_last_btn = btn;
    }
    return pop_evt();
}

}  // namespace pixfrog::ui::detail
