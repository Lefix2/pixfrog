// Host shim of the PARLIO TX driver: units and transmits are recorded so a
// test can decode what would have left the bus (shim::parlio_*).
#pragma once
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct parlio_tx_unit_t* parlio_tx_unit_handle_t;
typedef enum { PARLIO_CLK_SRC_DEFAULT = 0 } parlio_clock_source_t;
typedef enum { PARLIO_SAMPLE_EDGE_NEG = 0, PARLIO_SAMPLE_EDGE_POS = 1 } parlio_sample_edge_t;

typedef struct {
    parlio_clock_source_t clk_src;
    gpio_num_t clk_in_gpio_num;
    uint32_t input_clk_src_freq_hz;
    uint32_t output_clk_freq_hz;
    size_t data_width;
    gpio_num_t data_gpio_nums[16];
    gpio_num_t clk_out_gpio_num;
    gpio_num_t valid_gpio_num;
    size_t trans_queue_depth;
    size_t max_transfer_size;
    size_t dma_burst_size;
    parlio_sample_edge_t sample_edge;
} parlio_tx_unit_config_t;

typedef struct {
    uint32_t idle_value;
    struct {
        uint32_t queue_nonblocking : 1;
        uint32_t loop_transmission : 1;
    } flags;
} parlio_transmit_config_t;

esp_err_t parlio_new_tx_unit(const parlio_tx_unit_config_t* cfg, parlio_tx_unit_handle_t* out);
esp_err_t parlio_del_tx_unit(parlio_tx_unit_handle_t unit);
esp_err_t parlio_tx_unit_enable(parlio_tx_unit_handle_t unit);
esp_err_t parlio_tx_unit_disable(parlio_tx_unit_handle_t unit);
esp_err_t parlio_tx_unit_transmit(parlio_tx_unit_handle_t unit, const void* payload,
                                  size_t payload_bits, const parlio_transmit_config_t* cfg);
