#include "ui.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config_store.h"
#include "ui_internal.h"

namespace pixfrog::ui {

namespace {

constexpr const char* TAG = "UI";

SemaphoreHandle_t   g_wakeup_sem = nullptr;
TaskHandle_t        g_task       = nullptr;
InitConfig          g_cfg{};

void IRAM_ATTR encoder_isr(void*) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(g_wakeup_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

void task_main(void*) {
    detail::menu_init();
    detail::menu_render();
    detail::oled_flush();

    const TickType_t home_refresh = pdMS_TO_TICKS(100);  // 10 Hz HOME refresh
    const uint32_t   idle_ticks   = pdMS_TO_TICKS(config::get_global().home_timeout_s * 1000);
    TickType_t       last_event   = xTaskGetTickCount();

    while (true) {
        if (xSemaphoreTake(g_wakeup_sem, home_refresh) == pdTRUE) {
            detail::Event e;
            while ((e = detail::encoder_poll()) != detail::Event::None) {
                detail::menu_dispatch(e);
                last_event = xTaskGetTickCount();
            }
        } else {
            // Timer wake — refresh HOME content if applicable.
        }

        if ((xTaskGetTickCount() - last_event) > idle_ticks) {
            detail::menu_on_idle_timeout();
            last_event = xTaskGetTickCount();
        }

        if (detail::menu_is_dirty()) {
            detail::menu_render();
            detail::oled_flush();
            detail::menu_clear_dirty();
        }
    }
}

}  // namespace

bool start(const InitConfig& cfg) {
    g_cfg = cfg;
    g_wakeup_sem = xSemaphoreCreateBinary();
    if (!g_wakeup_sem) return false;

    if (!detail::oled_init(cfg.i2c_port, cfg.oled_addr)) {
        ESP_LOGE(TAG, "oled init failed");
        return false;
    }
    if (!detail::encoder_init(cfg.i2c_port, cfg.encoder_addr, cfg.encoder_int_gpio)) {
        ESP_LOGE(TAG, "encoder init failed");
        return false;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg.encoder_int_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(static_cast<gpio_num_t>(cfg.encoder_int_gpio), encoder_isr, nullptr);

    xTaskCreatePinnedToCore(task_main, "ui", 4096, nullptr, 4, &g_task, 0);
    ESP_LOGI(TAG, "started");
    return true;
}

}  // namespace pixfrog::ui
