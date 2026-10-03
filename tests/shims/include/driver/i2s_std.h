// Host shim: the I2S standard-mode subset audio.cpp uses. The channel records
// its configuration and every sample written (shim::i2s_log()).
#pragma once
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

typedef enum { I2S_NUM_0 = 0, I2S_NUM_1, I2S_NUM_2 } i2s_port_t;
typedef enum { I2S_ROLE_MASTER = 0, I2S_ROLE_SLAVE } i2s_role_t;
typedef enum { I2S_CLK_SRC_DEFAULT = 0 } i2s_clock_src_t;
typedef enum { I2S_MCLK_MULTIPLE_128 = 128, I2S_MCLK_MULTIPLE_256 = 256 } i2s_mclk_multiple_t;
typedef enum { I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_DATA_BIT_WIDTH_32BIT = 32 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_BIT_WIDTH_AUTO = 0 } i2s_slot_bit_width_t;
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef enum {
    I2S_STD_SLOT_LEFT  = 1,
    I2S_STD_SLOT_RIGHT = 2,
    I2S_STD_SLOT_BOTH  = 3
} i2s_std_slot_mask_t;
#define I2S_GPIO_UNUSED GPIO_NUM_NC

typedef struct i2s_channel_t* i2s_chan_handle_t;

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear_after_cb;  // zeros when nothing new (IDF alias: auto_clear)
} i2s_chan_config_t;

typedef struct {
    uint32_t sample_rate_hz;
    i2s_clock_src_t clk_src;
    i2s_mclk_multiple_t mclk_multiple;
} i2s_std_clk_config_t;

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
    i2s_std_slot_mask_t slot_mask;
    uint32_t ws_width;
    bool ws_pol;
    bool bit_shift;
} i2s_std_slot_config_t;

typedef struct {
    gpio_num_t mclk, bclk, ws, dout, din;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

esp_err_t i2s_new_channel(const i2s_chan_config_t* cfg, i2s_chan_handle_t* tx,
                          i2s_chan_handle_t* rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t* cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t h);
esp_err_t i2s_channel_disable(i2s_chan_handle_t h);
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void* src, size_t size, size_t* written,
                            uint32_t timeout_ms);
esp_err_t i2s_del_channel(i2s_chan_handle_t h);
