// Host shim: SPI panel IO. Commands and colour transfers are recorded
// (shim::lcd_log); on_color_trans_done fires at once after tx_color.
#pragma once
#include "esp_lcd_types.h"

typedef struct {
} esp_lcd_panel_io_event_data_t;

typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t io,
                                                       esp_lcd_panel_io_event_data_t* edata,
                                                       void* user_ctx);
typedef struct {
    int cs_gpio_num;
    int dc_gpio_num;
    int spi_mode;
    unsigned int pclk_hz;
    size_t trans_queue_depth;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void* user_ctx;
    int lcd_cmd_bits;
    int lcd_param_bits;
    uint8_t cs_ena_pretrans;
    uint8_t cs_ena_posttrans;
    struct {
        unsigned int dc_high_on_cmd : 1;
        unsigned int dc_low_on_data : 1;
        unsigned int dc_low_on_param : 1;
        unsigned int octal_mode : 1;
        unsigned int quad_mode : 1;
        unsigned int sio_mode : 1;
        unsigned int lsb_first : 1;
        unsigned int cs_high_active : 1;
    } flags;
} esp_lcd_panel_io_spi_config_t;

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t* cfg,
                                   esp_lcd_panel_io_handle_t* ret_io);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* param,
                                    size_t param_size);
esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io, int lcd_cmd, const void* color,
                                    size_t color_size);
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);
