// I2S standard mode: records the configuration and the samples written.
#include "driver/i2s_std.h"

#include <cstring>

#include "shim_control.h"

struct i2s_channel_t {
    int id;
};

namespace {
shim::I2sLog g_log;
}

namespace shim {
I2sLog& i2s_log() {
    return g_log;
}
void i2s_reset() {
    g_log = I2sLog{};
}
}  // namespace shim

esp_err_t i2s_new_channel(const i2s_chan_config_t* cfg, i2s_chan_handle_t* tx,
                          i2s_chan_handle_t* rx) {
    if (shim::should_fail(shim::Fault::I2sNew)) return ESP_FAIL;
    if (!cfg || (!tx && !rx)) return ESP_ERR_INVALID_ARG;
    if (tx) *tx = new i2s_channel_t{ cfg->id };
    if (rx) *rx = new i2s_channel_t{ cfg->id };
    ++g_log.channels;
    g_log.auto_clear = cfg->auto_clear_after_cb;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t* cfg) {
    if (!h || !cfg) return ESP_ERR_INVALID_ARG;
    g_log.sample_rate   = cfg->clk_cfg.sample_rate_hz;
    g_log.mclk_multiple = cfg->clk_cfg.mclk_multiple;
    g_log.mclk          = cfg->gpio_cfg.mclk;
    g_log.bclk          = cfg->gpio_cfg.bclk;
    g_log.ws            = cfg->gpio_cfg.ws;
    g_log.dout          = cfg->gpio_cfg.dout;
    return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t h) {
    if (!h) return ESP_ERR_INVALID_ARG;
    g_log.enabled = true;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t h) {
    if (!h) return ESP_ERR_INVALID_ARG;
    g_log.enabled = false;
    return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void* src, size_t size, size_t* written,
                            uint32_t) {
    if (shim::should_fail(shim::Fault::I2sWrite)) return ESP_FAIL;
    if (!h || !src || !g_log.enabled) return ESP_ERR_INVALID_STATE;
    const auto* s = static_cast<const int16_t*>(src);
    g_log.samples.insert(g_log.samples.end(), s, s + size / 2);
    if (written) *written = size;
    ++g_log.writes;
    if (g_log.on_write) g_log.on_write(g_log.writes, g_log.on_write_ctx);
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t h) {
    delete h;
    ++g_log.deleted;
    return ESP_OK;
}
