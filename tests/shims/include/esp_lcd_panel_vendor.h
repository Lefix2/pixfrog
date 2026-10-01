// Host shim: the vendor panel constructors the UI uses (ST7789).
#pragma once
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef struct {
    int reset_gpio_num;
    esp_lcd_color_space_t color_space;
    uint32_t bits_per_pixel;
    struct {
        uint32_t reset_active_high : 1;
    } flags;
    void* vendor_config;
} esp_lcd_panel_dev_config_t;

esp_err_t esp_lcd_new_panel_st7789(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t* cfg,
                                   esp_lcd_panel_handle_t* ret_panel);
