// Host shim: esp_lcd handle and enum types (see src/lcd.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

typedef struct esp_lcd_panel_t* esp_lcd_panel_handle_t;
typedef struct esp_lcd_panel_io_t* esp_lcd_panel_io_handle_t;
typedef int esp_lcd_spi_bus_handle_t;
typedef enum { LCD_CLK_SRC_DEFAULT = 0 } lcd_clock_source_t;
typedef enum { ESP_LCD_COLOR_SPACE_RGB = 0, ESP_LCD_COLOR_SPACE_BGR = 1 } esp_lcd_color_space_t;
