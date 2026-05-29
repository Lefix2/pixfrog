#include "ui.h"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config_store.h"
#include "ui_internal.h"

namespace pixfrog::ui {

namespace {

constexpr const char* TAG = "UI";

SemaphoreHandle_t       g_wakeup_sem = nullptr;
TaskHandle_t            g_task       = nullptr;
InitConfig              g_cfg{};
i2c_master_bus_handle_t g_bus        = nullptr;

uint32_t                g_ip_host       = 0;            // host-order IPv4; 0 = no link

void IRAM_ATTR encoder_isr(void*) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(g_wakeup_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

bool create_i2c_bus(const InitConfig& cfg) {
    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = cfg.i2c_port;
    bus_cfg.scl_io_num                   = static_cast<gpio_num_t>(cfg.i2c_scl_gpio);
    bus_cfg.sda_io_num                   = static_cast<gpio_num_t>(cfg.i2c_sda_gpio);
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void task_main(void*) {
    detail::menu_init();
    detail::menu_render();
    detail::oled_flush();

    const TickType_t home_refresh = pdMS_TO_TICKS(100);  // 10 Hz HOME refresh
    const uint32_t   idle_ms      = config::get_global().home_timeout_s * 1000u;
    const TickType_t idle_ticks   = pdMS_TO_TICKS(idle_ms ? idle_ms : 30000u);
    TickType_t       last_event   = xTaskGetTickCount();

    while (true) {
        if (xSemaphoreTake(g_wakeup_sem, home_refresh) == pdTRUE) {
            detail::Event e;
            while ((e = detail::encoder_poll()) != detail::Event::None) {
                detail::menu_dispatch(e);
                last_event = xTaskGetTickCount();
            }
        }
        if ((xTaskGetTickCount() - last_event) > idle_ticks) {
            detail::menu_on_idle_timeout();
            last_event = xTaskGetTickCount();
        }
        // Always re-render + flush; oled_flush is diff-based and writes
        // 0 bytes over I2C when the framebuffer is unchanged.
        detail::menu_render();
        detail::oled_flush();
    }
}

}  // namespace

bool start(const InitConfig& cfg) {
    g_cfg = cfg;
    g_wakeup_sem = xSemaphoreCreateBinary();
    if (!g_wakeup_sem) return false;

    if (!create_i2c_bus(cfg)) return false;

    if (!detail::oled_init(g_bus, cfg.oled_addr)) {
        ESP_LOGE(TAG, "oled init failed");
        return false;
    }
    if (!detail::encoder_init(g_bus, cfg.encoder_addr, cfg.encoder_int_gpio)) {
        ESP_LOGE(TAG, "encoder init failed");
        return false;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << cfg.encoder_int_gpio;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_NEGEDGE;
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(static_cast<gpio_num_t>(cfg.encoder_int_gpio), encoder_isr, nullptr);

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

}  // namespace pixfrog::ui
