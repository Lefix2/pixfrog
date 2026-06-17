// pixfrog — main entry point.
//
// Boot sequence:
//   1. NVS init + load config            (config_store)
//   2. Universe pool + pixel buffers     (dmx_manager)
//   3. LCD_CAM driver                    (led_output)
//   4. UI (OLED + encoder)               (ui)
//   5. Network: Ethernet + lwIP + events (main::init_network)
//   6. ArtNet UDP receiver               (artnet)
//   7. render_task spawn

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include "esp32_p4_devkit.h"

#include "artnet.h"
#include "config_store.h"
#include "control_console.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fseq_player.h"
#include "led_output.h"
#include "led_protocols.h"
#include "sacn.h"
#include "ui.h"
#include "web_config.h"

namespace {

constexpr const char* TAG = "MAIN";

esp_netif_t* g_eth_netif      = nullptr;
esp_eth_handle_t g_eth_handle = nullptr;

void publish_ip(uint32_t host_order_ip) {
    pixfrog::ui::set_ip(host_order_ip);
    pixfrog::artnet::set_local_ip(host_order_ip);
}

// Item 7: IP_EVENT_ETH_GOT_IP handler — fires after DHCP completes (or
// immediately when a static IP is configured + accepted by lwIP).
extern "C" void on_got_ip(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/,
                          void* event_data) {
    auto* event            = static_cast<ip_event_got_ip_t*>(event_data);
    const uint32_t host_ip = lwip_ntohl(event->ip_info.ip.addr);
    publish_ip(host_ip);
    ESP_LOGI(TAG, "GOT_IP %u.%u.%u.%u", static_cast<unsigned>((host_ip >> 24) & 0xFFu),
             static_cast<unsigned>((host_ip >> 16) & 0xFFu),
             static_cast<unsigned>((host_ip >> 8) & 0xFFu), static_cast<unsigned>(host_ip & 0xFFu));
}

extern "C" void on_eth_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t event_id,
                             void* /*event_data*/) {
    switch (event_id) {
    case ETHERNET_EVENT_START: ESP_LOGI(TAG, "Ethernet driver started"); break;
    case ETHERNET_EVENT_STOP: ESP_LOGI(TAG, "Ethernet driver stopped"); break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link UP");
        pixfrog::ui::set_link_up(true);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link DOWN");
        pixfrog::ui::set_link_up(false);
        publish_ip(0);
        break;
    default: break;
    }
}

// Items 5+6: bring up the MAC + IP101 PHY, attach to a netif, apply
// static IP or wait for DHCP, register event handlers, start the driver.
// Failures are logged but never abort boot — the UI still works without
// a network link, so the user can fix the config from the panel.
void init_network() {
    esp_netif_init();
    esp_event_loop_create_default();

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    g_eth_netif                  = esp_netif_new(&netif_cfg);
    if (!g_eth_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return;
    }

    // MAC config — RMII, MDC/MDIO pins from the board file.
    eth_mac_config_t mac_cfg             = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size           = 4096;
    eth_esp32_emac_config_t esp_emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_emac_cfg.smi_gpio.mdc_num        = pixfrog::board::kEthMdcGpio;
    esp_emac_cfg.smi_gpio.mdio_num       = pixfrog::board::kEthMdioGpio;
    esp_eth_mac_t* mac                   = esp_eth_mac_new_esp32(&esp_emac_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "esp_eth_mac_new_esp32 failed");
        return;
    }

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr         = pixfrog::board::kEthPhyAddress;
    phy_cfg.reset_gpio_num   = pixfrog::board::kEthPhyResetGpio;
    esp_eth_phy_t* phy       = esp_eth_phy_new_ip101(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "esp_eth_phy_new_ip101 failed");
        return;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_cfg, &g_eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_driver_install failed");
        return;
    }

    esp_netif_attach(g_eth_netif, esp_eth_new_netif_glue(g_eth_handle));

    // Item 6: static vs DHCP. If static is requested AND the static IP is
    // non-zero, stop DHCP and install the IP info ahead of link-up so
    // there's no transient DHCP attempt.
    const auto& g = pixfrog::config::get_global();
    if (!g.use_dhcp && g.static_ip != 0) {
        esp_netif_dhcpc_stop(g_eth_netif);
        esp_netif_ip_info_t ip_info{};
        ip_info.ip.addr      = lwip_htonl(g.static_ip);
        ip_info.netmask.addr = lwip_htonl(g.static_mask ? g.static_mask : 0xFFFFFF00u);
        ip_info.gw.addr      = lwip_htonl(g.static_gateway);
        esp_netif_set_ip_info(g_eth_netif, &ip_info);
        // No GOT_IP event fires in pure-static mode, so publish ourselves.
        publish_ip(g.static_ip);
        ESP_LOGI(TAG, "static IP %u.%u.%u.%u configured",
                 static_cast<unsigned>((g.static_ip >> 24) & 0xFFu),
                 static_cast<unsigned>((g.static_ip >> 16) & 0xFFu),
                 static_cast<unsigned>((g.static_ip >> 8) & 0xFFu),
                 static_cast<unsigned>(g.static_ip & 0xFFu));
    } else {
        ESP_LOGI(TAG, "DHCP enabled, awaiting lease");
    }

    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, nullptr);
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, nullptr);

    if (esp_eth_start(g_eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed");
        return;
    }
    ESP_LOGI(TAG, "Ethernet started");
}

void render_task(void*) {
    // Item 3: subscribe the render task to the task watchdog. The render
    // task is the canary for "the system is still meeting its real-time
    // budget" — if it stops kicking the WDT, we'd rather panic-reset than
    // ship dark/garbled frames silently.
    esp_task_wdt_add(nullptr);

    // Drift-corrected emission deadline (µs). Advancing it by exactly one period
    // each frame keeps the long-run rate at refresh_rate_hz with no integer-ms
    // truncation, and makes refresh_rate_hz a hard ceiling: ArtSync may wake the
    // wait early but we never emit before the deadline, so the observed frame
    // rate can't climb above the configured setting (which sizes the DMA budget).
    int64_t next_frame_us = esp_timer_get_time();

    // Item 4: rolling 1-second window FPS counter.
    int64_t fps_window_start_us   = esp_timer_get_time();
    uint32_t fps_frames_in_window = 0;

    while (true) {
        // Item A5: recompute the period every frame so a UI commit of
        // refresh_rate_hz takes effect on the next frame without a reboot.
        const uint8_t rate_hz   = pixfrog::config::get_global().refresh_rate_hz;
        const int64_t period_us = rate_hz ? (1'000'000LL / rate_hz) : 33'333LL;
        // Item 7: apply any config changes committed by the UI since the
        // last frame. Rebuilds universe→channel LUT, clears dirty bits.
        // No-op when nothing is pending.
        pixfrog::dmx::handle_pending_remaps();

        pixfrog::dmx::swap_universes();

        // TODO B5: when the UI has selected a calibration pattern, the
        // render loop emits that pattern instead of pixel data. This
        // makes scope debugging persistent across many frames without
        // requiring a recompile.
        const int8_t cal_mode = pixfrog::output::get_calibration_mode();
        if (cal_mode >= 0) {
            pixfrog::output::emit_calibration_pattern(static_cast<uint8_t>(cal_mode));
        } else {
            // Item 1: decode each channel's universes into its pixel back
            // buffer (DMX start offset, multi-universe spanning), then
            // swap the front pointer so the LCD_CAM encoder sees the new
            // pixels. Per-pixel transformations (color order, brightness,
            // grouping, invert) are applied later by led::encode_channel
            // during the LCD_CAM render.
            for (size_t ch = 0; ch < pixfrog::config::kNumChannels; ++ch) {
                pixfrog::dmx::decode_pixels_for_channel(ch);
                pixfrog::dmx::swap_pixels(ch);
            }
            pixfrog::output::render_frame();
        }

        // Item 4: publish FPS once per second.
        fps_frames_in_window++;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - fps_window_start_us >= 1'000'000) {
            pixfrog::dmx::set_current_fps(fps_frames_in_window);
            fps_frames_in_window = 0;
            fps_window_start_us  = now_us;
        }

        // Item 3: kick the WDT before sleeping. If render_frame ever blocks
        // past the WDT timeout (5 s default), we want a clean panic.
        esp_task_wdt_reset();

        // Pace to the next deadline. ArtSync wakes the wait early, but the loop
        // re-checks the deadline and keeps waiting until it passes: a sync aligns
        // the emit to the controller without ever pushing the rate above
        // refresh_rate_hz (the hard ceiling that protects the DMA budget).
        //
        // Never pace faster than the frame physically emits, either: a channel
        // whose wire time exceeds the period (a DMX universe at 60 Hz sits at
        // its ~44 Hz, ~22.7 ms ceiling) must emit intact at its own rate rather
        // than be over-submitted. The interval is max(period, longest channel).
        const int64_t emit_us      = static_cast<int64_t>(pixfrog::dmx::frame_emit_us());
        const int64_t interval_us  = period_us > emit_us ? period_us : emit_us;
        next_frame_us             += interval_us;
        int64_t pace_us            = esp_timer_get_time();
        // A slow frame that overran a whole interval must not spiral into a burst
        // of catch-up frames — resync the deadline to now instead.
        if (pace_us - next_frame_us > interval_us) next_frame_us = pace_us;
        while (pace_us < next_frame_us) {
            const TickType_t wait = pdMS_TO_TICKS((next_frame_us - pace_us + 999) / 1000);
            pixfrog::dmx::wait_for_sync_or_period(wait ? wait : 1);
            pace_us = esp_timer_get_time();
        }
    }
}

}  // namespace

// On the Waveshare ESP32-P4 module, the VDD_IO_5 pad domain (GPIO39-48 —
// carries LED bus CH5 CLOCK and CH7 DATA/CLOCK) is wired to the P4's internal
// LDO output VO4, not to 3.3 V. Left unprogrammed, VO4 idles near 1.2 V and
// those outputs swing 0-1.2 V instead of 0-3.3 V (measured on GPIO47).
// Acquire the channel at 3.3 V before any LED output starts and never
// release it. VDD_IO_6 (GPIO49-54) is tied to 3.3 V on the module and needs
// nothing.
void power_vdd_io5_pads() {
    esp_ldo_channel_config_t cfg = {
        .chan_id    = 4,
        .voltage_mv = 3300,
    };
    static esp_ldo_channel_handle_t s_chan = nullptr;
    const esp_err_t err                    = esp_ldo_acquire_channel(&cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LDO VO4 acquire failed (%s) — GPIO39-48 stuck at ~1.2V",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "LDO VO4 → 3.3V (VDD_IO_5 pads GPIO39-48)");
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "pixfrog booting on %s", pixfrog::board::kBoardName);

    power_vdd_io5_pads();

    pixfrog::config::init();
    if (!pixfrog::dmx::init()) {
        ESP_LOGE(TAG, "dmx_manager init failed — aborting");
        return;
    }

    {
        pixfrog::fseq::InitConfig sd_cfg;
        sd_cfg.clk_gpio = pixfrog::board::kSdmmcClkGpio;
        sd_cfg.cmd_gpio = pixfrog::board::kSdmmcCmdGpio;
        sd_cfg.d0_gpio  = pixfrog::board::kSdmmcD0Gpio;
        sd_cfg.d1_gpio  = pixfrog::board::kSdmmcD1Gpio;
        sd_cfg.d2_gpio  = pixfrog::board::kSdmmcD2Gpio;
        sd_cfg.d3_gpio  = pixfrog::board::kSdmmcD3Gpio;
        pixfrog::fseq::init(sd_cfg);
    }

    pixfrog::output::InitConfig lcd_cfg{
        .bus_gpio_16           = pixfrog::board::kLedBusGpio,
        .pclk_hz               = pixfrog::led::kPclkHz,
        .max_samples_per_frame = pixfrog::led::kMaxSamplesPerFrame,
    };
    if (!pixfrog::output::init(lcd_cfg)) {
        ESP_LOGE(TAG, "lcd_cam init failed — aborting");
        return;
    }

    pixfrog::ui::InitConfig ui_cfg{
        .i2c_port           = pixfrog::board::kI2cPort,
        .i2c_sda_gpio       = pixfrog::board::kI2cSdaGpio,
        .i2c_scl_gpio       = pixfrog::board::kI2cSclGpio,
        .i2c_freq_hz        = pixfrog::board::kI2cFreqHz,
        .encoder_addr       = pixfrog::board::kEncoderI2cAddr,
        .oled_addr          = pixfrog::board::kOledI2cAddr,
        .spi_host           = pixfrog::board::kDisplaySpiHost,
        .spi_clk_gpio       = pixfrog::board::kDisplayClkGpio,
        .spi_mosi_gpio      = pixfrog::board::kDisplayMosiGpio,
        .spi_cs_gpio        = pixfrog::board::kDisplayCsGpio,
        .tft_dc_gpio        = pixfrog::board::kDisplayDcGpio,
        .tft_rst_gpio       = pixfrog::board::kDisplayRstGpio,
        .spi_freq_hz        = pixfrog::board::kDisplaySpiFreqHz,
        .tft_width          = 320,  // landscape addressing (swap_xy in tft_init)
        .tft_height         = 240,
        .tft_backlight_gpio = pixfrog::board::kDisplayBacklightGpio,
    };
    pixfrog::ui::start(ui_cfg);

    init_network();
    pixfrog::artnet::start();

    if (pixfrog::config::get_global().sacn_enabled) pixfrog::sacn::start();
    if (pixfrog::config::get_global().fpp_remote) pixfrog::fpp::start();
    if (pixfrog::config::get_global().boot_scene > 0)
        pixfrog::dmx::scene_start(pixfrog::config::get_global().boot_scene - 1);
    if (pixfrog::config::get_global().web_enabled) pixfrog::web::start();

    xTaskCreatePinnedToCore(render_task, "render", 6144, nullptr, 20, nullptr, 1);

    pixfrog::console::start();

    // OTA rollback gate: every subsystem above came up, so confirm this
    // image. If a freshly OTA'd build crashes before reaching this line,
    // the bootloader reverts to the previous slot on the next reset.
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image confirmed on %s", running->label);
    }

    ESP_LOGI(TAG, "boot complete (%s)", running->label);
}
