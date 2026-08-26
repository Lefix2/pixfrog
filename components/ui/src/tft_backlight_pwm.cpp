// Backlight backend — LEDC PWM on the panel's BL pin.
//
// The ER-TFT2.79-1 exposes the bare LED string (LEDA/LEDK, 5 chips, Vf 3.0 V,
// If typ 75 mA): the pad sources that current, out of the VDD_IO_5 domain fed
// by internal LDO VO4 — the same domain as the SDMMC pads. Two consequences:
//   • 100 % duty leaves the pad statically high, exactly as the pre-dimming
//     firmware drove it, so dimming can only ever lower the average current;
//   • 20 kHz keeps the switching out of the audio band and above any camera
//     shutter, and the pad's drive strength is left alone — on this panel it is
//     what sets the LED current, so touching it would move full brightness.

#include "ui_internal.h"

#include "driver/ledc.h"
#include "esp_log.h"

namespace pixfrog::ui::detail {

namespace {

constexpr const char* TAG = "BL";

constexpr ledc_mode_t kMode         = LEDC_LOW_SPEED_MODE;  // the P4 has no high-speed mode
constexpr ledc_timer_t kTimer       = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel   = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kResBits = LEDC_TIMER_10_BIT;
constexpr uint32_t kFreqHz          = 20'000;  // 10 bits × 20 kHz = 20.5 MHz, well under 80 MHz
constexpr uint32_t kFullDuty        = (1u << 10) - 1;
constexpr uint32_t kMinDuty         = kFullDuty / 100;  // 1 % — the dimmest still-even step
constexpr uint32_t kFadeMs          = 400;

int g_gpio  = -1;
bool g_fade = false;  // fade service installed

uint32_t duty_for(uint8_t pct) {
    if (pct == 0) return 0;
    if (pct > 100) pct = 100;
    // LED current is linear in duty, perceived brightness is not: square the
    // level (gamma ≈ 2) so consecutive steps look evenly spaced.
    const uint32_t d = (static_cast<uint32_t>(pct) * pct * kFullDuty) / 10'000u;
    return d < kMinDuty ? kMinDuty : d;
}

}  // namespace

void backlight_backend_init(int gpio) {
    g_gpio = gpio;
    if (g_gpio < 0) return;  // BL hard-wired on — nothing to drive

    ledc_timer_config_t t{};
    t.speed_mode      = kMode;
    t.timer_num       = kTimer;
    t.duty_resolution = kResBits;
    t.freq_hz         = kFreqHz;
    t.clk_cfg         = LEDC_AUTO_CLK;
    esp_err_t err     = ledc_timer_config(&t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(err));
        g_gpio = -1;
        return;
    }

    ledc_channel_config_t c{};
    c.gpio_num   = g_gpio;
    c.speed_mode = kMode;
    c.channel    = kChannel;
    c.timer_sel  = kTimer;
    c.intr_type  = LEDC_INTR_DISABLE;
    c.duty       = 0;  // dark until the first frame is flushed
    c.hpoint     = 0;
    err          = ledc_channel_config(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(err));
        g_gpio = -1;
        return;
    }

    g_fade = (ledc_fade_func_install(0) == ESP_OK);
    if (!g_fade) ESP_LOGW(TAG, "fade service unavailable — brightness will step");
}

void backlight_backend_set(uint8_t pct, bool fade) {
    if (g_gpio < 0) return;
    const uint32_t duty = duty_for(pct);
    if (fade && g_fade) {
        ledc_set_fade_time_and_start(kMode, kChannel, duty, kFadeMs, LEDC_FADE_NO_WAIT);
        return;
    }
    // A fade still running would keep writing duty behind our back; stopping it
    // is a no-op when the channel is idle.
    if (g_fade) ledc_fade_stop(kMode, kChannel);
    ledc_set_duty(kMode, kChannel, duty);
    ledc_update_duty(kMode, kChannel);
}

}  // namespace pixfrog::ui::detail
