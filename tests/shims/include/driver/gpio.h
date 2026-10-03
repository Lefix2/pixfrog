// Host shim: GPIO calls are counted, levels remembered (shim::gpio_level).
#pragma once
#include "esp_err.h"

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;

#include <cstdint>

enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 };
enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 };
enum { GPIO_INTR_DISABLE = 0 };

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t* cfg);
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t pin, unsigned level);
int gpio_get_level(gpio_num_t pin);  // shim::gpio_input(), 1 by default (pulled up)
