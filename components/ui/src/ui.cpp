#include "ui.h"

#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "ui_internal.h"

namespace pixfrog::ui {

namespace {

constexpr const char* TAG = "UI";

TaskHandle_t g_task = nullptr;
InitConfig g_cfg{};
i2c_master_bus_handle_t g_bus = nullptr;

uint32_t g_ip_host   = 0;      // host-order IPv4; 0 = no link
bool g_link_up       = false;  // ETH_EVENT_CONNECTED state
NetState g_net_state = NetState::Disconnected;

bool create_i2c_bus(const InitConfig& cfg) {
    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = cfg.i2c_port;
    bus_cfg.scl_io_num                   = static_cast<gpio_num_t>(cfg.i2c_scl_gpio);
    bus_cfg.sda_io_num                   = static_cast<gpio_num_t>(cfg.i2c_sda_gpio);
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    esp_err_t err                        = i2c_new_master_bus(&bus_cfg, &g_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void task_main(void*) {
    detail::menu_init();

    // Boot splash — runs until done or user clicks.
    {
        const TickType_t t0 = xTaskGetTickCount();
        bool done           = false;
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
        bool bl_on = false;
#endif
        while (!done) {
            const uint32_t t_ms    = static_cast<uint32_t>((xTaskGetTickCount() - t0) *
                                                           portTICK_PERIOD_MS);
            const detail::Event ev = detail::encoder_poll();
            detail::encoder_led_tick();  // breathe during the splash
            done = detail::splash_render(t_ms, ev == detail::Event::Click);
#ifdef CONFIG_PIXFROG_DISPLAY_TFT
            // First frame is now on the panel — safe to light the backlight
            // (kept off through boot so the white power-on state never shows).
            if (!bl_on) {
                detail::tft_backlight(true);
                bl_on = true;
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(33));  // ~30 fps
        }
    }

    detail::menu_render();
    detail::canvas_flush();

    const TickType_t home_refresh = pdMS_TO_TICKS(33);  // ~30 Hz: smooth LED breath/fade
    const uint32_t idle_ms        = config::get_global().home_timeout_s * 1000u;
    const TickType_t idle_ticks   = pdMS_TO_TICKS(idle_ms ? idle_ms : 30000u);
    TickType_t last_event         = xTaskGetTickCount();

    while (true) {
        // Time-based polling at ~30 Hz: the loop runs at this rate anyway for
        // the encoder-LED animation and the diff-based display refresh, so a
        // one-tick worst-case input latency costs nothing perceptible.
        vTaskDelay(home_refresh);
        detail::Event e;
        while ((e = detail::encoder_poll()) != detail::Event::None) {
            detail::menu_dispatch(e);
            last_event = xTaskGetTickCount();
            // LED mode follows the screen: breathe on HOME, full green + yellow
            // action-blips in config. set_active() before flash() so the blip on
            // the click that opens config registers.
            detail::encoder_led_set_active(!detail::menu_is_home());
            detail::encoder_led_flash();
        }
        if ((xTaskGetTickCount() - last_event) > idle_ticks) {
            detail::menu_on_idle_timeout();
            last_event = xTaskGetTickCount();
        }
        detail::encoder_led_set_active(!detail::menu_is_home());
        detail::encoder_led_tick();
        // Always re-render + flush; oled_flush is diff-based and writes
        // 0 bytes over I2C when the framebuffer is unchanged.
        detail::menu_render();
        detail::canvas_flush();
    }
}

}  // namespace

namespace detail {

uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

const char* fw_version() {
    return esp_app_get_description()->version;
}

const char* fw_build_info() {
    static char buf[64];
    if (buf[0] == '\0') {
        const esp_app_desc_t* app = esp_app_get_description();
        std::snprintf(buf, sizeof(buf), "IDF %s  %s", esp_get_idf_version(), app->date);
    }
    return buf;
}

}  // namespace detail

bool start(const InitConfig& cfg) {
    g_cfg = cfg;

    if (!create_i2c_bus(cfg)) return false;

#ifdef CONFIG_PIXFROG_DISPLAY_TFT
    {
        detail::TftConfig tft_cfg{};
        tft_cfg.spi_host       = cfg.spi_host;
        tft_cfg.clk_gpio       = cfg.spi_clk_gpio;
        tft_cfg.mosi_gpio      = cfg.spi_mosi_gpio;
        tft_cfg.cs_gpio        = cfg.spi_cs_gpio;
        tft_cfg.dc_gpio        = cfg.tft_dc_gpio;
        tft_cfg.rst_gpio       = cfg.tft_rst_gpio;
        tft_cfg.freq_hz        = cfg.spi_freq_hz;
        tft_cfg.width          = cfg.tft_width;
        tft_cfg.height         = cfg.tft_height;
        tft_cfg.backlight_gpio = cfg.tft_backlight_gpio;
        if (!detail::tft_init(tft_cfg)) {
            ESP_LOGE(TAG, "tft init failed");
            return false;
        }
    }
#else
    if (!detail::oled_init(g_bus, cfg.oled_addr)) {
        ESP_LOGE(TAG, "oled init failed");
        return false;
    }
#endif
    if (!detail::encoder_init(g_bus, cfg.encoder_addr)) {
        ESP_LOGE(TAG, "encoder init failed");
        return false;
    }
    detail::encoder_led_init();

    xTaskCreatePinnedToCore(task_main, "ui", 4096, nullptr, 4, &g_task, 0);
    ESP_LOGI(TAG, "started");
    return true;
}

void set_ip(uint32_t host_order_ip) {
    g_ip_host = host_order_ip;
}

uint32_t get_ip() {
    return g_ip_host;
}

void set_link_up(bool up) {
    g_link_up = up;
}

bool is_link_up() {
    return g_link_up;
}

void set_net_state(NetState s) {
    g_net_state = s;
}

NetState get_net_state() {
    return g_net_state;
}

}  // namespace pixfrog::ui
