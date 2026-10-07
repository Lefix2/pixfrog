// pixfrog — main entry point.
//
// Boot sequence:
//   1. NVS init + load config            (config_store)
//   2. Universe pool + pixel buffers     (dmx_manager)
//   3. LED bus output driver             (led_output)
//   4. UI (OLED + encoder)               (ui)
//   5. Network: Ethernet + lwIP (main::init_network), addressing (net)
//   6. ArtNet UDP receiver               (artnet)
//   7. frame pipeline: compose_task (core 0) + render_task (core 1)

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include <cstring>

#include "esp32_p4_devkit.h"

// OLED builds define no TFT geometry (Kconfig `depends on`); the two InitConfig
// fields are unused on that path.
#ifndef CONFIG_PIXFROG_TFT_WIDTH
#define CONFIG_PIXFROG_TFT_WIDTH 0
#define CONFIG_PIXFROG_TFT_HEIGHT 0
#endif

#include "artnet.h"
#include "audio.h"
#include "config_store.h"
#include "control_console.h"
#include "dmx_manager.h"
#include "fpp_sync.h"
#include "fseq_player.h"
#include "led_output.h"
#include "led_protocols.h"
#include "net.h"
#include "sacn.h"
#include "ui.h"
#include "web_config.h"

namespace {

constexpr const char* TAG = "MAIN";

esp_netif_t* g_eth_netif      = nullptr;
esp_eth_handle_t g_eth_handle = nullptr;

// What the box advertises: the UI's link / address / state and Art-Net's
// local address all follow the net component.
void publish_net(uint32_t host_order_ip, pixfrog::net::State st) {
    pixfrog::ui::set_link_up(st != pixfrog::net::State::Disconnected);
    pixfrog::ui::set_ip(host_order_ip);
    pixfrog::artnet::set_local_ip(host_order_ip);
    pixfrog::ui::set_net_state(
        st == pixfrog::net::State::Disconnected ? pixfrog::ui::NetState::Disconnected
        : st == pixfrog::net::State::Connected  ? pixfrog::ui::NetState::Connected
                                                : pixfrog::ui::NetState::Acquiring);
}

// Bring up the MAC + IP101 PHY, attach to a netif, apply
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

    // Static or DHCP, the link and address events: the net component, which
    // the web, the console, the menu and ArtIpProg also call to re-address
    // the box live.
    pixfrog::net::init(g_eth_netif, publish_net);

    if (esp_eth_start(g_eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_eth_start failed");
        return;
    }
    ESP_LOGI(TAG, "Ethernet started");
}

// ── The frame pipeline ──────────────────────────────────────────────────────
// compose_task (core 0) makes frame N+1 — universes swapped, the control
// universe read, every output drawn (scenes, effects, groups, fixtures) into
// the pixel back buffers — while render_task (core 1) encodes frame N from the
// fronts and hands it to the DMA. Drawing and encoding each get a core, so a
// frame costs the longer of the two instead of their sum. The fronts change
// hands through two binary semaphores: compose publishes (swaps every output's
// pixels) only once the encoder is done reading them, then says a frame is
// ready. All the drawing state (clocks, fades, the plays' snapshot) stays with
// one task, compose, as it stayed with render before.
SemaphoreHandle_t g_frame_ready = nullptr;  // a composed frame waits in the fronts
SemaphoreHandle_t g_fronts_free = nullptr;  // the encoder is done with the fronts

// The longest a pipeline task blocks before it kicks the watchdog and looks
// again: a stalled partner shows as a frozen frame rate, not a reset.
constexpr TickType_t kPipelineWaitTicks = pdMS_TO_TICKS(200);

void compose_task(void*) {
    // On the task watchdog too: a compose that stops is as dark as a render
    // that stops.
    esp_task_wdt_add(nullptr);

    // Drift-corrected emission deadline (µs). Advancing it by exactly one period
    // each frame keeps the long-run rate at refresh_rate_hz with no integer-ms
    // truncation, and makes refresh_rate_hz a hard ceiling: ArtSync may wake the
    // wait early but we never emit before the deadline, so the observed frame
    // rate can't climb above the configured setting (which sizes the DMA budget).
    int64_t next_frame_us = esp_timer_get_time();

    // The longest decode of the window, and of the one before (shown while
    // this one fills). Static: the host harness restarts the task body.
    static int64_t window_start_us  = esp_timer_get_time();
    static uint32_t decode_max_us   = 0;
    static uint32_t decode_max_last = 0;
#if configGENERATE_RUN_TIME_STATS
    // Core 0's load, read here on core 0: FreeRTOS only adds to the idle
    // task's run time when it is switched out, and it just made way for us.
    // (render_task reads core 1's.)
    static configRUN_TIME_COUNTER_TYPE idle_at = ulTaskGetIdleRunTimeCounterForCore(0);
#endif

    while (true) {
        // Recompute the period every frame so a UI commit of
        // refresh_rate_hz takes effect on the next frame without a reboot.
        const uint8_t rate_hz   = pixfrog::config::get_global().refresh_rate_hz;
        const int64_t period_us = rate_hz ? (1'000'000LL / rate_hz) : 33'333LL;
        // Apply any config changes committed by the UI since the
        // last frame. Rebuilds universe→channel LUT, clears dirty bits.
        // No-op when nothing is pending.
        pixfrog::dmx::handle_pending_remaps();

        pixfrog::dmx::swap_universes();
        // The DMX control universe (master, blackout, strobe, scenes, fades)
        // reads the bank just published.
        pixfrog::dmx::update_show_control();

        // Decode each channel's universes into its pixel back buffer (DMX
        // start offset, multi-universe spanning) — or the scene, the effects,
        // the fixtures it shows. Per-pixel transformations (color order,
        // brightness, grouping, invert) are applied later by
        // led::encode_channel during the frame encode. A calibration pattern
        // replaces the frame on the encoder's side: nothing to draw.
        if (pixfrog::output::get_calibration_mode() < 0) {
            const int64_t decode_from = esp_timer_get_time();
            for (size_t ch = 0; ch < pixfrog::config::kNumChannels; ++ch)
                pixfrog::dmx::decode_pixels_for_channel(ch);
            const auto decode_us = static_cast<uint32_t>(esp_timer_get_time() - decode_from);
            if (decode_us > decode_max_us) decode_max_us = decode_us;
            pixfrog::dmx::set_decode_time(
                decode_us, decode_max_us > decode_max_last ? decode_max_us : decode_max_last);
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us - window_start_us >= 1'000'000) {
#if configGENERATE_RUN_TIME_STATS
            const configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(0);
            pixfrog::dmx::set_cpu_load(
                0, pixfrog::dmx::cpu_load_pct(static_cast<uint32_t>(idle - idle_at),
                                              static_cast<uint32_t>(now_us - window_start_us)));
            idle_at = idle;
#endif
            decode_max_last = decode_max_us;
            decode_max_us   = 0;
            window_start_us = now_us;
        }

        // Hand the frame over once the encoder has let go of the fronts.
        while (xSemaphoreTake(g_fronts_free, kPipelineWaitTicks) != pdTRUE)
            esp_task_wdt_reset();
        pixfrog::dmx::publish_pixels();
        xSemaphoreGive(g_frame_ready);
        esp_task_wdt_reset();

        // Pace to the next deadline. ArtSync wakes the wait early, but the loop
        // re-checks the deadline and keeps waiting until it passes: a sync aligns
        // the emit to the controller without ever pushing the rate above
        // refresh_rate_hz (the hard ceiling that protects the DMA budget).
        //
        // Never pace faster than the frame physically emits, either: a channel
        // whose wire time exceeds the period (a long strip at a high refresh)
        // must emit intact at its own rate rather than be over-submitted. The interval is
        // max(period, longest channel).
        const int64_t emit_us      = static_cast<int64_t>(pixfrog::dmx::frame_emit_us());
        const int64_t interval_us  = period_us > emit_us ? period_us : emit_us;
        next_frame_us             += interval_us;
        int64_t pace_us            = esp_timer_get_time();
        // A slow frame that overran a whole interval must not spiral into a burst
        // of catch-up frames — resync the deadline to now instead.
        if (pace_us - next_frame_us > interval_us) next_frame_us = pace_us;
        bool waited = false;
        while (pace_us < next_frame_us) {
            const TickType_t wait = pdMS_TO_TICKS((next_frame_us - pace_us + 999) / 1000);
            pixfrog::dmx::wait_for_sync_or_period(wait ? wait : 1);
            pace_us = esp_timer_get_time();
            waited  = true;
        }
        // Drawing longer than the period leaves no wait at all: give core 0 a
        // tick anyway, or the UI, the console and the idle task (its watchdog)
        // starve behind compose. The frame rate drops instead.
        if (!waited) vTaskDelay(1);
    }
}

void render_task(void*) {
    // Subscribe the render task to the task watchdog. The render
    // task is the canary for "the system is still meeting its real-time
    // budget" — if it stops kicking the WDT, we'd rather panic-reset than
    // ship dark/garbled frames silently.
    esp_task_wdt_add(nullptr);

    // Rolling 1-second window FPS counter, and core 1's idle time at its start
    // (the CPU load). Static: the host harness restarts the task body.
    static int64_t fps_window_start_us = esp_timer_get_time();
#if configGENERATE_RUN_TIME_STATS
    configRUN_TIME_COUNTER_TYPE idle_at = ulTaskGetIdleRunTimeCounterForCore(1);
#endif
    static uint32_t fps_frames_in_window = 0;

    while (true) {
        // The next composed frame. None for a while (compose stalled, or
        // pacing a slow refresh): kick the watchdog and wait again.
        if (xSemaphoreTake(g_frame_ready, kPipelineWaitTicks) == pdTRUE) {
            // When the UI has selected a calibration pattern, the
            // render loop emits that pattern instead of pixel data. This
            // makes scope debugging persistent across many frames without
            // requiring a recompile.
            const int8_t cal_mode = pixfrog::output::get_calibration_mode();
            if (cal_mode >= 0)
                pixfrog::output::emit_calibration_pattern(static_cast<uint8_t>(cal_mode));
            else
                pixfrog::output::render_frame();
            // Encoded: compose may publish the next frame into the fronts.
            xSemaphoreGive(g_fronts_free);
            fps_frames_in_window++;
        }

        // Publish FPS once per second.
        const int64_t now_us = esp_timer_get_time();
        if (now_us - fps_window_start_us >= 1'000'000) {
            pixfrog::dmx::set_current_fps(fps_frames_in_window);
#if configGENERATE_RUN_TIME_STATS
            // Core 1's load, read here on core 1 (compose_task reads core 0's).
            const configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(1);
            pixfrog::dmx::set_cpu_load(
                1, pixfrog::dmx::cpu_load_pct(static_cast<uint32_t>(idle - idle_at),
                                              static_cast<uint32_t>(now_us - fps_window_start_us)));
            idle_at = idle;
#endif
            fps_frames_in_window = 0;
            fps_window_start_us  = now_us;
        }

        // Kick the WDT. If render_frame ever blocks past the WDT timeout
        // (5 s default), we want a clean panic.
        esp_task_wdt_reset();
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
        .flags      = {},
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

// The speaker needs the R52 mod (docs/HARDWARE.md §2.7): stock, GPIO53 (LED
// CH8 DATA) drives the amp enable through R52 (0 Ω) into R58, 10 kΩ to
// ground, so the amp would follow the LED data. With R52 removed the pin sees
// only the CH8 line driver input: the internal pull-up (~45 kΩ) reads it high,
// where the stock 10 kΩ divider holds it near 0.6 V, low. Probed before the
// LED outputs take the pin. (It cannot see the PA_CTRL pull-up itself.)
bool speaker_mod_present() {
    const int pin         = pixfrog::board::kAmpProbeGpio;
    const gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    esp_rom_delay_us(200);
    const bool high = gpio_get_level(static_cast<gpio_num_t>(pin)) == 1;
    gpio_reset_pin(static_cast<gpio_num_t>(pin));
    ESP_LOGI(TAG, "speaker: R52 %s (GPIO%d reads %s)", high ? "removed" : "fitted — stock board",
             pin, high ? "high" : "low");
    return high;
}

// A new OTA image boots "pending verify": if it resets before confirming
// itself, the bootloader marks it invalid and boots the previous slot. That is
// the only trace of a rollback, so turn it into a logged, persisted record the
// web UI and the console can show until someone acknowledges it.
// Fixed-size, possibly unterminated IDF strings → a terminated, truncated copy.
void copy_field(char* dst, size_t cap, const char* src, size_t src_cap) {
    const size_t n = strnlen(src, src_cap);
    const size_t k = n < cap - 1 ? n : cap - 1;
    std::memcpy(dst, src, k);
    dst[k] = '\0';
}

void record_rollback_if_any() {
    const esp_partition_t* bad = esp_ota_get_last_invalid_partition();
    if (!bad) return;
    esp_app_desc_t desc{};
    if (esp_ota_get_partition_description(bad, &desc) != ESP_OK) return;

    pixfrog::config::RollbackRecord prev{};
    const bool known = pixfrog::config::get_rollback(prev) &&
                       std::memcmp(prev.rejected_sha, desc.app_elf_sha256,
                                   sizeof(prev.rejected_sha)) == 0;
    if (known) {
        if (!prev.acknowledged)
            ESP_LOGW(TAG, "OTA rollback (unacknowledged): %s on %s was rejected, running %s",
                     prev.rejected_version, prev.rejected_slot, prev.running_version);
        return;
    }
    pixfrog::config::RollbackRecord rec{};
    copy_field(rec.rejected_version, sizeof(rec.rejected_version), desc.version,
               sizeof(desc.version));
    copy_field(rec.rejected_slot, sizeof(rec.rejected_slot), bad->label, sizeof(bad->label));
    copy_field(rec.running_version, sizeof(rec.running_version), esp_app_get_description()->version,
               sizeof(desc.version));
    std::memcpy(rec.rejected_sha, desc.app_elf_sha256, sizeof(rec.rejected_sha));
    rec.reset_reason = static_cast<uint8_t>(esp_reset_reason());
    pixfrog::config::set_rollback(rec);
    ESP_LOGW(TAG, "OTA ROLLBACK: firmware %s on %s was rejected (reset reason %d) — running %s",
             rec.rejected_version, rec.rejected_slot, rec.reset_reason, rec.running_version);
}

// Confirm a pending-verify image only once it has proven itself: 30 s of a
// live render loop. Confirming at the end of app_main (as before) happened
// before a single frame rendered, so an image crashing a few seconds in was
// already "valid" and boot-looped instead of rolling back. No network
// condition: a box booted without its cable must not reject a good image.
constexpr uint32_t kOtaConfirmDelayMs = 30'000;

void ota_confirm_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(kOtaConfirmDelayMs));
    for (int tries = 0; tries < 6; ++tries) {
        if (pixfrog::dmx::get_stats().current_fps > 0) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA image confirmed after %u s of healthy rendering",
                     static_cast<unsigned>(kOtaConfirmDelayMs / 1000 + tries * 5));
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGE(TAG, "render loop not running 60 s after an OTA boot — rebooting to roll back");
    esp_restart();
}

extern "C" void app_main() {
    // Capture logs from the very first line so the web Diagnostics tab has the
    // boot log even when the web server stays disabled (tees to UART as usual).
    pixfrog::web::init_log_capture();
    ESP_LOGI(TAG, "pixfrog booting on %s", pixfrog::board::kBoardName);

    power_vdd_io5_pads();

    pixfrog::config::init();
    record_rollback_if_any();
    if (!pixfrog::dmx::init()) {
        ESP_LOGE(TAG, "dmx_manager init failed — aborting");
        return;
    }

    {
        pixfrog::fseq::InitConfig sd_cfg;
        sd_cfg.clk_gpio   = pixfrog::board::kSdmmcClkGpio;
        sd_cfg.cmd_gpio   = pixfrog::board::kSdmmcCmdGpio;
        sd_cfg.d0_gpio    = pixfrog::board::kSdmmcD0Gpio;
        sd_cfg.d1_gpio    = pixfrog::board::kSdmmcD1Gpio;
        sd_cfg.d2_gpio    = pixfrog::board::kSdmmcD2Gpio;
        sd_cfg.d3_gpio    = pixfrog::board::kSdmmcD3Gpio;
        sd_cfg.power_gpio = pixfrog::board::kSdPowerGpio;
        pixfrog::fseq::init(sd_cfg);
    }

    const bool speaker_ok = speaker_mod_present();

    pixfrog::output::InitConfig out_cfg{
        .bus_gpio_16           = pixfrog::board::kLedBusGpio,
        .pclk_hz               = pixfrog::led::kPclkHz,
        .max_samples_per_frame = pixfrog::led::kMaxSamplesPerFrame,
    };
    if (!pixfrog::output::init(out_cfg)) {
        ESP_LOGE(TAG, "led_output init failed — aborting");
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
        .tft_width          = CONFIG_PIXFROG_TFT_WIDTH,   // landscape logical size — the
        .tft_height         = CONFIG_PIXFROG_TFT_HEIGHT,  // panel driver owns the rotation
        .tft_backlight_gpio = pixfrog::board::kDisplayBacklightGpio,
    };
    pixfrog::ui::start(ui_cfg);

    // Speaker: the ES8311 shares the UI's I2C bus. No codec = no sound, nothing else.
    pixfrog::audio::InitConfig audio_cfg{};
    audio_cfg.i2c_bus    = pixfrog::ui::i2c_bus();
    audio_cfg.codec_addr = pixfrog::board::kCodecI2cAddr;
    audio_cfg.mclk_gpio  = pixfrog::board::kI2sMclkGpio;
    audio_cfg.bclk_gpio  = pixfrog::board::kI2sBclkGpio;
    audio_cfg.ws_gpio    = pixfrog::board::kI2sWsGpio;
    audio_cfg.dout_gpio  = pixfrog::board::kI2sDoutGpio;
    audio_cfg.din_gpio   = pixfrog::board::kI2sDinGpio;
    audio_cfg.volume     = [] {
        return pixfrog::config::speaker_volume_pct(pixfrog::config::get_global());
    };
    // The UI plays the boot croak near the end of its splash.
    if (speaker_ok) pixfrog::ui::set_speaker_present(pixfrog::audio::init(audio_cfg));

    init_network();
    pixfrog::artnet::start();

    // Many sACN universes outgrow the EMAC's multicast filter: pass them all.
    pixfrog::sacn::set_multicast_overflow_hook([](bool pass_all) {
        if (g_eth_handle) esp_eth_ioctl(g_eth_handle, ETH_CMD_S_ALL_MULTICAST, &pass_all);
    });
    if (pixfrog::config::get_global().sacn_enabled) pixfrog::sacn::start();
    if (pixfrog::config::get_global().fpp_remote) pixfrog::fpp::start();
    if (pixfrog::config::get_global().boot_scene > 0)
        pixfrog::dmx::scene_start_on(pixfrog::config::get_global().boot_scene - 1,
                                     pixfrog::dmx::kAllOutputs, 0);  // lit at once
    if (pixfrog::config::get_global().web_enabled) pixfrog::web::start();

    // The frame pipeline: compose on core 0, just under the receivers (Art-Net
    // and sACN at 10, the EMAC and lwIP above) so a burst of universes is never
    // held back by drawing; encode alone on core 1.
    g_frame_ready = xSemaphoreCreateBinary();
    g_fronts_free = xSemaphoreCreateBinary();
    if (!g_frame_ready || !g_fronts_free) {
        ESP_LOGE(TAG, "frame pipeline semaphores: out of memory");
        return;
    }
    xSemaphoreGive(g_fronts_free);  // nothing encoding yet
    // render only encodes now (~3 kB used on the bench): internal RAM is what
    // the output's DMA lists need.
    xTaskCreatePinnedToCore(render_task, "render", 4608, nullptr, 20, nullptr, 1);
    xTaskCreatePinnedToCore(compose_task, "compose", 4096, nullptr, 9, nullptr, 0);

    pixfrog::console::start();

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA image on %s pending verification (%u s)", running->label,
                 static_cast<unsigned>(kOtaConfirmDelayMs / 1000));
        xTaskCreatePinnedToCore(ota_confirm_task, "ota_confirm", 3072, nullptr, 3, nullptr, 0);
    }

    ESP_LOGI(TAG, "boot complete (%s)", running->label);
}
