// Host shim: I2C master. Devices are models the test attaches at an address
// (shim::i2c_attach); an address with no model NACKs.
#pragma once
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

typedef int i2c_port_num_t;
typedef struct i2c_master_bus_t* i2c_master_bus_handle_t;
typedef struct i2c_master_dev_t* i2c_master_dev_handle_t;
typedef enum { I2C_CLK_SRC_DEFAULT = 0 } i2c_clock_source_t;
typedef enum { I2C_ADDR_BIT_LEN_7 = 0, I2C_ADDR_BIT_LEN_10 = 1 } i2c_addr_bit_len_t;

typedef struct {
    i2c_port_num_t i2c_port;
    gpio_num_t sda_io_num;
    gpio_num_t scl_io_num;
    i2c_clock_source_t clk_source;
    uint8_t glitch_ignore_cnt;
    int intr_priority;
    size_t trans_queue_depth;
    struct {
        uint32_t enable_internal_pullup : 1;
        uint32_t allow_pd : 1;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    uint32_t scl_wait_us;
    struct {
        uint32_t disable_ack_check : 1;
    } flags;
} i2c_device_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* cfg, i2c_master_bus_handle_t* ret);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t* cfg,
                                    i2c_master_dev_handle_t* ret);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t* data, size_t len,
                              int timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t* data, size_t len,
                             int timeout_ms);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t addr, int timeout_ms);
