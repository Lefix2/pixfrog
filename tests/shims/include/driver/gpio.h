// Host shim: GPIO calls are counted, levels remembered (shim::gpio_level).
#pragma once
#include "esp_err.h"

typedef int gpio_num_t;
#define GPIO_NUM_NC (-1)
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;

esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t pin, unsigned level);
