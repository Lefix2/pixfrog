// I2C master, LEDC and ROM delay on the host: the UI's display/encoder
// hardware. I2C devices are models the test attaches (shim::i2c_attach).
#include <map>

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"
#include "shim_control.h"

struct i2c_master_bus_t {
    i2c_master_bus_config_t cfg;
};
struct i2c_master_dev_t {
    uint16_t addr;
};

namespace {
std::map<uint16_t, shim::I2cDevice*> g_devices;
int g_probe_nacks = 0;
shim::LedcLog g_ledc;
}  // namespace

namespace shim {
void i2c_attach(uint16_t addr, I2cDevice* dev) {
    if (dev)
        g_devices[addr] = dev;
    else
        g_devices.erase(addr);
}
void i2c_nack_probes(int n) {
    g_probe_nacks = n;
}
LedcLog& ledc_log() {
    return g_ledc;
}
void ledc_reset() {
    g_ledc = LedcLog{};
}
}  // namespace shim

// ── I2C ─────────────────────────────────────────────────────────────────────

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* cfg, i2c_master_bus_handle_t* ret) {
    if (shim::should_fail(shim::Fault::I2cBus)) return ESP_FAIL;
    if (!cfg || !ret) return ESP_ERR_INVALID_ARG;
    *ret = new i2c_master_bus_t{ *cfg };
    return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t* cfg,
                                    i2c_master_dev_handle_t* ret) {
    if (shim::should_fail(shim::Fault::I2cAddDevice)) return ESP_FAIL;
    if (!bus || !cfg || !ret) return ESP_ERR_INVALID_ARG;
    *ret = new i2c_master_dev_t{ cfg->device_address };
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t* data, size_t len, int) {
    if (shim::should_fail(shim::Fault::I2cTransmit)) return ESP_FAIL;
    if (!dev || !data || !len) return ESP_ERR_INVALID_ARG;
    auto it = g_devices.find(dev->addr);
    if (it == g_devices.end() || !it->second->write(data, len)) return ESP_FAIL;  // NACK
    return ESP_OK;
}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t* data, size_t len, int) {
    if (shim::should_fail(shim::Fault::I2cReceive)) return ESP_FAIL;
    if (!dev || !data || !len) return ESP_ERR_INVALID_ARG;
    auto it = g_devices.find(dev->addr);
    if (it == g_devices.end() || !it->second->read(data, len)) return ESP_FAIL;
    return ESP_OK;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int) {
    if (!bus) return ESP_ERR_INVALID_ARG;
    if (g_probe_nacks > 0) {
        --g_probe_nacks;
        return ESP_ERR_NOT_FOUND;
    }
    return g_devices.count(addr) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void esp_rom_delay_us(uint32_t us) {
    shim::advance_us(us);
}

// ── LEDC ────────────────────────────────────────────────────────────────────

esp_err_t ledc_timer_config(const ledc_timer_config_t* cfg) {
    if (shim::should_fail(shim::Fault::LedcTimer)) return ESP_FAIL;
    g_ledc.freq_hz  = cfg->freq_hz;
    g_ledc.res_bits = cfg->duty_resolution;
    return ESP_OK;
}
esp_err_t ledc_channel_config(const ledc_channel_config_t* cfg) {
    if (shim::should_fail(shim::Fault::LedcChannel)) return ESP_FAIL;
    g_ledc.gpio = cfg->gpio_num;
    g_ledc.duty = cfg->duty;
    return ESP_OK;
}
esp_err_t ledc_fade_func_install(int) {
    if (shim::should_fail(shim::Fault::LedcFade)) return ESP_FAIL;
    g_ledc.fade_installed = true;
    return ESP_OK;
}
esp_err_t ledc_set_fade_time_and_start(ledc_mode_t, ledc_channel_t, uint32_t duty,
                                       uint32_t max_fade_time_ms, ledc_fade_mode_t) {
    if (!g_ledc.fade_installed) return ESP_ERR_INVALID_STATE;
    g_ledc.duty         = duty;  // where the fade lands
    g_ledc.last_fade_ms = max_fade_time_ms;
    ++g_ledc.fades;
    return ESP_OK;
}
esp_err_t ledc_fade_stop(ledc_mode_t, ledc_channel_t) {
    if (!g_ledc.fade_installed) return ESP_ERR_INVALID_STATE;
    ++g_ledc.fade_stops;
    return ESP_OK;
}
esp_err_t ledc_set_duty(ledc_mode_t, ledc_channel_t, uint32_t duty) {
    g_ledc.pending_duty = duty;
    return ESP_OK;
}
esp_err_t ledc_update_duty(ledc_mode_t, ledc_channel_t) {
    g_ledc.duty = g_ledc.pending_duty;
    ++g_ledc.steps;
    return ESP_OK;
}
