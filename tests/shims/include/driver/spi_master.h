// Host shim: SPI bus initialisation (each host can be initialised once).
#pragma once
#include <cstdint>

#include "esp_err.h"

typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;
#define SPI_DMA_CH_AUTO 3

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int data4_io_num;
    int data5_io_num;
    int data6_io_num;
    int data7_io_num;
    int max_transfer_sz;
    uint32_t flags;
    int intr_flags;
} spi_bus_config_t;

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t* cfg, int dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host);
