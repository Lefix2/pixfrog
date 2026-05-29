// pixfrog — main entry point.
// Boot sequence:
//   1. NVS init + load config
//   2. Pixel buffers + universe pool allocation (dmx_manager)
//   3. LCD_CAM driver init (lcd_cam_output)
//   4. UI init (ui)        — needs config_store for default home_timeout
//   5. Network init (Ethernet + lwIP)
//   6. ArtNet receiver start
//   7. render_task spawn

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp32_p4_devkit.h"

#include "artnet.h"
#include "config_store.h"
#include "dmx_manager.h"
#include "lcd_cam_output.h"
#include "led_protocols.h"
#include "ui.h"

namespace {

constexpr const char* TAG = "MAIN";

void init_network() {
    esp_netif_init();
    esp_event_loop_create_default();
    // TODO: configure RMII EMAC + IP101 PHY:
    //   eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    //   eth_esp32_emac_config_t esp_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    //   esp_cfg.smi_mdc_gpio_num  = pixfrog::board::kEthMdcGpio;
    //   esp_cfg.smi_mdio_gpio_num = pixfrog::board::kEthMdioGpio;
    //   eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    //   phy_cfg.phy_addr   = pixfrog::board::kEthPhyAddress;
    //   phy_cfg.reset_gpio_num = pixfrog::board::kEthPhyResetGpio;
    //   esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&esp_cfg, &mac_cfg);
    //   esp_eth_phy_t* phy = esp_eth_phy_new_ip101(&phy_cfg);
    //   esp_eth_config_t eth = ETH_DEFAULT_CONFIG(mac, phy);
    //   esp_eth_handle_t handle = nullptr;
    //   esp_eth_driver_install(&eth, &handle);
    //   …apply static IP from config if !use_dhcp…
    //   esp_eth_start(handle);
    ESP_LOGW(TAG, "network bring-up TODO");
}

void render_task(void*) {
    const TickType_t period = pdMS_TO_TICKS(1000 / pixfrog::config::get_global().refresh_rate_hz);
    TickType_t       last   = xTaskGetTickCount();
    while (true) {
        pixfrog::dmx::swap_universes();
        // TODO: decode each channel's front universes into pixel_back_buffer(ch)
        //       per ChannelConfig (DMX start offset, color order, brightness, …).
        for (size_t ch = 0; ch < pixfrog::config::kNumChannels; ++ch) {
            pixfrog::dmx::swap_pixels(ch);
        }
        pixfrog::lcd::render_frame();
        vTaskDelayUntil(&last, period);
    }
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(TAG, "pixfrog booting on %s", pixfrog::board::kBoardName);

    pixfrog::config::init();
    if (!pixfrog::dmx::init()) {
        ESP_LOGE(TAG, "dmx_manager init failed — aborting");
        return;
    }

    pixfrog::lcd::InitConfig lcd_cfg{
        .bus_gpio_16           = pixfrog::board::kLedBusGpio,
        .pclk_hz               = pixfrog::led::kPclkHz,
        .max_samples_per_frame = pixfrog::led::kMaxSamplesPerFrame,
    };
    if (!pixfrog::lcd::init(lcd_cfg)) {
        ESP_LOGE(TAG, "lcd_cam init failed — aborting");
        return;
    }

    pixfrog::ui::InitConfig ui_cfg{
        .i2c_port         = pixfrog::board::kI2cPort,
        .i2c_sda_gpio     = pixfrog::board::kI2cSdaGpio,
        .i2c_scl_gpio     = pixfrog::board::kI2cSclGpio,
        .i2c_freq_hz      = pixfrog::board::kI2cFreqHz,
        .encoder_int_gpio = pixfrog::board::kEncoderIntGpio,
        .oled_addr        = pixfrog::board::kOledI2cAddr,
        .encoder_addr     = pixfrog::board::kEncoderI2cAddr,
    };
    pixfrog::ui::start(ui_cfg);

    init_network();
    pixfrog::artnet::start();

    xTaskCreatePinnedToCore(render_task, "render", 6144, nullptr, 20, nullptr, 1);

    ESP_LOGI(TAG, "boot complete");
}
