// esp_lcd + SPI bus + cache sync on the host: record what the LED backend and
// the UI drivers ask of the LCD hardware (shim::lcd_log).
#include <cstdarg>
#include <cstring>
#include <vector>

#include "driver/spi_master.h"
#include "esp_cache.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_vendor.h"
#include "shim_control.h"

struct esp_lcd_panel_t {
    bool rgb   = false;  // LCD_CAM RGB panel, else a SPI vendor panel
    bool ready = false;  // reset + init done
    esp_lcd_rgb_panel_config_t cfg{};
    std::vector<uint16_t> fbs[2];
    esp_lcd_rgb_panel_event_callbacks_t cbs{};
    void* user_ctx = nullptr;
};

struct esp_lcd_panel_io_t {
    esp_lcd_panel_io_spi_config_t cfg{};
};

namespace {
shim::LcdLog g_log;
bool g_auto_vsync  = true;
bool g_spi_host[3] = {};
}  // namespace

namespace shim {
LcdLog& lcd_log() {
    return g_log;
}
void lcd_reset() {
    g_log        = LcdLog{};
    g_auto_vsync = true;
    for (bool& h : g_spi_host)
        h = false;
}
void lcd_auto_vsync(bool on) {
    g_auto_vsync = on;
}
}  // namespace shim

// ── RGB panel ───────────────────────────────────────────────────────────────

esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* cfg,
                                esp_lcd_panel_handle_t* ret_panel) {
    if (shim::should_fail(shim::Fault::LcdNewPanel)) return ESP_FAIL;
    if (!cfg || !ret_panel || cfg->num_fbs < 1 || cfg->num_fbs > 2) return ESP_ERR_INVALID_ARG;
    auto* p              = new esp_lcd_panel_t;
    p->rgb               = true;
    p->cfg               = *cfg;
    const size_t samples = static_cast<size_t>(cfg->timings.h_res) * cfg->timings.v_res;
    for (size_t i = 0; i < cfg->num_fbs; ++i)
        p->fbs[i].assign(samples, 0xA5A5);  // not zeroed, like fresh PSRAM
    ++g_log.panels_created;
    g_log.pclk_hz = cfg->timings.pclk_hz;
    g_log.h_res   = cfg->timings.h_res;
    g_log.v_res   = cfg->timings.v_res;
    for (int i = 0; i < 16; ++i)
        g_log.data_gpios[i] = cfg->data_gpio_nums[i];
    *ret_panel = p;
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel,
                                                     const esp_lcd_rgb_panel_event_callbacks_t* cbs,
                                                     void* user_ctx) {
    if (!panel || !panel->rgb || !cbs) return ESP_ERR_INVALID_ARG;
    panel->cbs      = *cbs;
    panel->user_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, uint32_t fb_num,
                                             void** fb0, ...) {
    if (shim::should_fail(shim::Fault::LcdFrameBuffer)) return ESP_FAIL;
    if (!panel || !panel->rgb || fb_num > panel->cfg.num_fbs) return ESP_ERR_INVALID_ARG;
    *fb0 = panel->fbs[0].data();
    if (fb_num == 2) {
        va_list ap;
        va_start(ap, fb0);
        *va_arg(ap, void**) = panel->fbs[1].data();
        va_end(ap);
    }
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_refresh(esp_lcd_panel_handle_t panel) {
    if (shim::should_fail(shim::Fault::LcdRefresh)) return ESP_FAIL;
    if (!panel || !panel->rgb || !panel->ready) return ESP_ERR_INVALID_STATE;
    ++g_log.refreshes;
    const auto* f = static_cast<const uint16_t*>(g_log.last_drawn);
    g_log.last_frame.assign(f, f + panel->fbs[0].size());
    if (g_auto_vsync && panel->cbs.on_vsync) {
        ++g_log.vsyncs;
        panel->cbs.on_vsync(panel, nullptr, panel->user_ctx);
    }
    return ESP_OK;
}

// ── Generic panel ops ───────────────────────────────────────────────────────

esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel) {
    if (shim::should_fail(shim::Fault::LcdPanelReset)) return ESP_FAIL;
    return panel ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel) {
    if (shim::should_fail(shim::Fault::LcdPanelInit)) return ESP_FAIL;
    if (!panel) return ESP_ERR_INVALID_ARG;
    panel->ready = true;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel) {
    if (!panel) return ESP_ERR_INVALID_ARG;
    delete panel;
    ++g_log.panels_deleted;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end, const void* color_data) {
    if (shim::should_fail(shim::Fault::LcdDraw)) return ESP_FAIL;
    if (!panel || !color_data || x_end <= x_start || y_end <= y_start) return ESP_ERR_INVALID_ARG;
    ++g_log.draws;
    g_log.last_drawn = color_data;
    g_log.last_x1    = x_start;
    g_log.last_y1    = y_start;
    g_log.last_x2    = x_end;
    g_log.last_y2    = y_end;
    if (panel->rgb) {
        // Only the no-copy FB swap is modelled: drawing anything but a whole
        // frame buffer is a bug in the LED backend.
        if (color_data != panel->fbs[0].data() && color_data != panel->fbs[1].data())
            return ESP_ERR_INVALID_ARG;
        if (static_cast<uint32_t>(x_end) != panel->cfg.timings.h_res ||
            static_cast<uint32_t>(y_end) != panel->cfg.timings.v_res)
            return ESP_ERR_INVALID_ARG;
        if (panel->cbs.on_color_trans_done)
            panel->cbs.on_color_trans_done(panel, nullptr, panel->user_ctx);
    }
    return ESP_OK;
}
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y) {
    if (!panel) return ESP_ERR_INVALID_ARG;
    g_log.mirror_x = mirror_x;
    g_log.mirror_y = mirror_y;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes) {
    if (!panel) return ESP_ERR_INVALID_ARG;
    g_log.swap_xy = swap_axes;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert) {
    if (!panel) return ESP_ERR_INVALID_ARG;
    g_log.inverted = invert;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on) {
    if (!panel) return ESP_ERR_INVALID_ARG;
    g_log.display_on = on;
    return ESP_OK;
}

// ── SPI panel IO + vendor panels ────────────────────────────────────────────

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t* cfg, int) {
    if (shim::should_fail(shim::Fault::SpiBus)) return ESP_FAIL;
    if (!cfg || host < SPI1_HOST || host > SPI3_HOST) return ESP_ERR_INVALID_ARG;
    if (g_spi_host[host]) return ESP_ERR_INVALID_STATE;
    g_spi_host[host] = true;
    ++g_log.spi_buses;
    return ESP_OK;
}
esp_err_t spi_bus_free(spi_host_device_t host) {
    if (host < SPI1_HOST || host > SPI3_HOST || !g_spi_host[host]) return ESP_ERR_INVALID_STATE;
    g_spi_host[host] = false;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t* cfg,
                                   esp_lcd_panel_io_handle_t* ret_io) {
    if (shim::should_fail(shim::Fault::LcdNewIo)) return ESP_FAIL;
    if (!cfg || !ret_io || bus < SPI1_HOST || bus > SPI3_HOST || !g_spi_host[bus])
        return ESP_ERR_INVALID_ARG;
    *ret_io = new esp_lcd_panel_io_t{ *cfg };
    ++g_log.ios_created;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* param,
                                    size_t param_size) {
    if (!io || (param_size && !param)) return ESP_ERR_INVALID_ARG;
    g_log.commands.push_back(lcd_cmd);
    return ESP_OK;
}
esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* color,
                                    size_t color_size) {
    if (!io || !color || !color_size) return ESP_ERR_INVALID_ARG;
    g_log.commands.push_back(lcd_cmd);
    ++g_log.color_tx;
    if (io->cfg.on_color_trans_done) io->cfg.on_color_trans_done(io, nullptr, io->cfg.user_ctx);
    return ESP_OK;
}
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) {
    delete io;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t* cfg,
                                   esp_lcd_panel_handle_t* ret_panel) {
    if (shim::should_fail(shim::Fault::LcdNewPanel)) return ESP_FAIL;
    if (!io || !cfg || !ret_panel || cfg->bits_per_pixel != 16) return ESP_ERR_INVALID_ARG;
    *ret_panel = new esp_lcd_panel_t;
    ++g_log.panels_created;
    return ESP_OK;
}

// ── Cache ───────────────────────────────────────────────────────────────────

esp_err_t esp_cache_msync(void* addr, size_t size, int flags) {
    if (shim::should_fail(shim::Fault::CacheMsync)) return ESP_FAIL;
    if (!addr || !size) return ESP_ERR_INVALID_ARG;
    // C2M needs cache-line-granular sizes (the backend rounds to 128 B).
    if ((flags & ESP_CACHE_MSYNC_FLAG_DIR_C2M) && !(flags & ESP_CACHE_MSYNC_FLAG_UNALIGNED) &&
        size % 64)
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
