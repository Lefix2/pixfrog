// Host shim: LEDC PWM, recorded (shim::ledc_log).
#pragma once
#include <cstdint>

#include "esp_err.h"

typedef enum { LEDC_LOW_SPEED_MODE = 0 } ledc_mode_t;
typedef enum { LEDC_TIMER_0 = 0, LEDC_TIMER_1, LEDC_TIMER_2, LEDC_TIMER_3 } ledc_timer_t;
typedef enum { LEDC_CHANNEL_0 = 0, LEDC_CHANNEL_1 } ledc_channel_t;
typedef enum {
    LEDC_TIMER_8_BIT  = 8,
    LEDC_TIMER_10_BIT = 10,
    LEDC_TIMER_13_BIT = 13
} ledc_timer_bit_t;
typedef enum { LEDC_AUTO_CLK = 0 } ledc_clk_cfg_t;
typedef enum { LEDC_INTR_DISABLE = 0, LEDC_INTR_FADE_END } ledc_intr_type_t;
typedef enum { LEDC_FADE_NO_WAIT = 0, LEDC_FADE_WAIT_DONE } ledc_fade_mode_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
    bool deconfigure;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
    struct {
        unsigned int output_invert : 1;
    } flags;
} ledc_channel_config_t;

esp_err_t ledc_timer_config(const ledc_timer_config_t* cfg);
esp_err_t ledc_channel_config(const ledc_channel_config_t* cfg);
esp_err_t ledc_fade_func_install(int intr_alloc_flags);
esp_err_t ledc_set_fade_time_and_start(ledc_mode_t mode, ledc_channel_t ch, uint32_t duty,
                                       uint32_t max_fade_time_ms, ledc_fade_mode_t wait);
esp_err_t ledc_fade_stop(ledc_mode_t mode, ledc_channel_t ch);
esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t ch, uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t ch);
