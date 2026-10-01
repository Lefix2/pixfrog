// Host shim: the LCD_CAM RGB panel. Frame buffers are real host memory; a
// refresh snapshots the drawn buffer and (unless shim::lcd_auto_vsync(false))
// fires on_vsync at once, as if the frame had been clocked out.
#pragma once
#include "esp_lcd_panel_ops.h"

typedef struct {
    uint32_t pclk_hz;
    uint32_t h_res;
    uint32_t v_res;
    uint32_t hsync_pulse_width;
    uint32_t hsync_back_porch;
    uint32_t hsync_front_porch;
    uint32_t vsync_pulse_width;
    uint32_t vsync_back_porch;
    uint32_t vsync_front_porch;
    struct {
        uint32_t hsync_idle_low : 1;
        uint32_t vsync_idle_low : 1;
        uint32_t de_idle_high : 1;
        uint32_t pclk_active_neg : 1;
        uint32_t pclk_idle_high : 1;
    } flags;
} esp_lcd_rgb_timing_t;

typedef struct {
    lcd_clock_source_t clk_src;
    esp_lcd_rgb_timing_t timings;
    size_t data_width;
    size_t bits_per_pixel;
    size_t num_fbs;
    size_t bounce_buffer_size_px;
    size_t sram_trans_align;
    size_t psram_trans_align;
    int hsync_gpio_num;
    int vsync_gpio_num;
    int de_gpio_num;
    int pclk_gpio_num;
    int disp_gpio_num;
    int data_gpio_nums[16];
    struct {
        uint32_t disp_active_low : 1;
        uint32_t refresh_on_demand : 1;
        uint32_t fb_in_psram : 1;
        uint32_t double_fb : 1;
        uint32_t no_fb : 1;
        uint32_t bb_invalidate_cache : 1;
    } flags;
} esp_lcd_rgb_panel_config_t;

typedef struct {
} esp_lcd_rgb_panel_event_data_t;

typedef bool (*esp_lcd_rgb_panel_general_cb_t)(esp_lcd_panel_handle_t panel,
                                               const esp_lcd_rgb_panel_event_data_t* edata,
                                               void* user_ctx);
typedef struct {
    esp_lcd_rgb_panel_general_cb_t on_color_trans_done;
    esp_lcd_rgb_panel_general_cb_t on_vsync;
    esp_lcd_rgb_panel_general_cb_t on_bounce_empty;
    esp_lcd_rgb_panel_general_cb_t on_bounce_frame_finish;
} esp_lcd_rgb_panel_event_callbacks_t;

esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* cfg,
                                esp_lcd_panel_handle_t* ret_panel);
esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel,
                                                     const esp_lcd_rgb_panel_event_callbacks_t* cbs,
                                                     void* user_ctx);
esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, uint32_t fb_num,
                                             void** fb0, ...);
esp_err_t esp_lcd_rgb_panel_refresh(esp_lcd_panel_handle_t panel);
