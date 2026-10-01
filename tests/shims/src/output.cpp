// PARLIO TX + GPIO on the host: record what the LED output backend asks of the
// hardware. One TX unit exists at a time, like on the P4.
#include <cstring>
#include <map>
#include <vector>

#include "driver/gpio.h"
#include "driver/parlio_tx.h"
#include "shim_control.h"

struct parlio_tx_unit_t {
    parlio_tx_unit_config_t cfg;
    bool enabled;
};

namespace {
parlio_tx_unit_t* g_unit = nullptr;
shim::ParlioLog g_log;
std::map<int, unsigned> g_levels;
int g_gpio_calls = 0;
}  // namespace

namespace shim {
ParlioLog& parlio_log() {
    return g_log;
}
void parlio_reset() {
    g_log = ParlioLog{};
}
int gpio_calls() {
    return g_gpio_calls;
}
unsigned gpio_level(int pin) {
    return g_levels.count(pin) ? g_levels[pin] : 0;
}
}  // namespace shim

esp_err_t parlio_new_tx_unit(const parlio_tx_unit_config_t* cfg, parlio_tx_unit_handle_t* out) {
    if (shim::should_fail(shim::Fault::ParlioNew) || g_unit) return ESP_FAIL;
    g_unit = new parlio_tx_unit_t{ *cfg, false };
    ++g_log.units_created;
    g_log.max_transfer_size = cfg->max_transfer_size;
    g_log.clk_hz            = cfg->output_clk_freq_hz;
    for (int i = 0; i < 16; ++i)
        g_log.data_gpios[i] = cfg->data_gpio_nums[i];
    *out = g_unit;
    return ESP_OK;
}
esp_err_t parlio_del_tx_unit(parlio_tx_unit_handle_t unit) {
    if (!unit || unit != g_unit || unit->enabled) return ESP_FAIL;  // must be disabled first
    delete g_unit;
    g_unit = nullptr;
    ++g_log.units_deleted;
    return ESP_OK;
}
esp_err_t parlio_tx_unit_enable(parlio_tx_unit_handle_t unit) {
    if (shim::should_fail(shim::Fault::ParlioEnable)) return ESP_FAIL;
    unit->enabled = true;
    return ESP_OK;
}
esp_err_t parlio_tx_unit_disable(parlio_tx_unit_handle_t unit) {
    unit->enabled = false;
    return ESP_OK;
}
esp_err_t parlio_tx_unit_transmit(parlio_tx_unit_handle_t unit, const void* payload,
                                  size_t payload_bits, const parlio_transmit_config_t* cfg) {
    if (shim::should_fail(shim::Fault::ParlioTransmit)) return ESP_FAIL;
    if (!unit->enabled || payload_bits % 16 || payload_bits / 8 > unit->cfg.max_transfer_size)
        return ESP_ERR_INVALID_ARG;
    ++g_log.transmits;
    g_log.loop        = cfg->flags.loop_transmission;
    g_log.last_buffer = payload;
    const auto* s     = static_cast<const uint16_t*>(payload);
    g_log.last_samples.assign(s, s + payload_bits / 16);
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t*) {
    ++g_gpio_calls;
    return ESP_OK;
}
esp_err_t gpio_reset_pin(gpio_num_t) {
    ++g_gpio_calls;
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) {
    ++g_gpio_calls;
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, unsigned level) {
    ++g_gpio_calls;
    g_levels[pin] = level;
    return ESP_OK;
}
