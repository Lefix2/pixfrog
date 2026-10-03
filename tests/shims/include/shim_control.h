// Test-side control of the IDF shims: fake clock, in-memory NVS with fault
// injection, log verbosity. Only the harness tests include this header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace shim {

// ── Clock (esp_timer_get_time, FreeRTOS ticks) ──────────────────────────────
void set_time_us(int64_t t);
void advance_us(int64_t dt);
void advance_ms(int64_t dt);
int64_t now_us();
// Serve mode (browser tests): time follows the wall clock, blocking calls sleep.
void use_real_clock(bool on);
bool real_clock();

// ── NVS ─────────────────────────────────────────────────────────────────────
void nvs_wipe();  // empty flash, faults cleared
bool nvs_has(const std::string& ns, const std::string& key);
std::vector<uint8_t> nvs_raw(const std::string& ns, const std::string& key);
void nvs_put_raw(const std::string& ns, const std::string& key, const void* data, size_t len);
void nvs_erase_key(const std::string& ns, const std::string& key);
// Fault injection: the next N calls of that kind fail (0 = never).
void nvs_fail_init(int count);   // nvs_flash_init returns an error
void nvs_fail_erase(int count);  // nvs_flash_erase returns an error
void nvs_fail_open(int count);   // nvs_open returns an error
int nvs_writes();                // nvs_set_blob calls since the last wipe

// ── Tasks ───────────────────────────────────────────────────────────────────
// xTaskCreate* records the task instead of spawning it; the test runs its
// body on the calling thread (it must return — see net_on_idle).
bool run_task(const char* name);
// Runs a task that never returns (a monitor loop) until it has blocked
// `max_delays` times (vTaskDelay / vTaskDelayUntil), then unwinds back here.
// `on_delay`, when set, runs at every one of those blocking points. `keep`
// leaves the task registered so a test can run it again (from the top).
bool run_task_for(const char* name, int max_delays, void (*on_delay)() = nullptr,
                  bool keep = false);
bool task_created(const char* name);
void tasks_forget();

// ── Network fabric (lwip/sockets.h) ─────────────────────────────────────────
struct Datagram {
    uint32_t ip;  // host order
    uint16_t port;
    std::vector<uint8_t> bytes;
};
// Queue a datagram for the socket bound to `port`, from `from_ip` (host order).
void net_push(uint16_t port, const std::vector<uint8_t>& bytes, uint32_t from_ip = 0xC0A80201);
// Called when a bound socket's queue is empty (the receive would block):
// typically the component's stop(), so its task loop returns to the test.
void net_on_idle(uint16_t port, void (*fn)());
std::vector<Datagram>& net_sent();                // everything sendto() emitted, oldest first
std::vector<uint32_t> net_groups(uint16_t port);  // joined multicast groups (host order)
void net_reset();

// ── Console / system ────────────────────────────────────────────────────────
// Runs one console line through the registered command table as the REPL
// would; returns the command's exit code and fills `out` with its stdout.
int console_exec(const char* line, std::string* out = nullptr);
int restarts();   // esp_restart() calls
int log_level();  // last esp_log_level_set("*", …)

// ── HTTP (esp_http_server) ──────────────────────────────────────────────────
struct HttpResponse {
    int status = 0;  // parsed from the status line, 200 when unset
    std::string status_line;
    std::string content_type;
    std::map<std::string, std::string> headers;
    std::string body;
    bool handled = false;  // a registered handler matched
};
HttpResponse http_request(const char* method, const std::string& uri,
                          const std::string& body                           = std::string(),
                          const std::map<std::string, std::string>& headers = {});
// Largest chunk one httpd_req_recv() returns (0 = whole body): exercises the
// handlers' body-reassembly loops.
void http_recv_chunk(size_t max);
bool http_running();
size_t http_routes();  // handlers registered (esp_http_server caps it)
// Serves the registered handlers on a real TCP port until *stop is set
// (browser tests); requests are dispatched on the calling thread.
void http_serve(uint16_t port, volatile bool* stop);
// WebSocket clients (esp_http_server WS subset): open one on a websocket
// route (its handler sees the handshake GET), read the frames pushed to it.
int ws_open(const std::string& uri);  // -1: no websocket route there
std::vector<std::string> ws_frames(int fd);
void ws_close(int fd);

// ── Fault injection ─────────────────────────────────────────────────────────
// The next `count` calls of that API fail (error paths of the code under test).
enum class Fault {
    Socket,          // socket() → -1
    Bind,            // bind() → -1
    Join,            // setsockopt(IP_ADD_MEMBERSHIP) → -1
    SendTo,          // sendto() → -1
    HeapCaps,        // heap_caps_calloc/malloc → nullptr
    Semaphore,       // xSemaphoreCreate* → nullptr
    EventGroup,      // xEventGroupCreate → nullptr
    OtaNoTarget,     // esp_ota_get_next_update_partition → nullptr
    OtaBegin,        // esp_ota_begin → ESP_FAIL
    OtaWrite,        // esp_ota_write → ESP_FAIL
    OtaSetBoot,      // esp_ota_set_boot_partition → ESP_FAIL
    HttpdStart,      // httpd_start → ESP_FAIL
    TaskCreate,      // xTaskCreate* → pdFAIL
    ParlioNew,       // parlio_new_tx_unit → ESP_FAIL
    ParlioEnable,    // parlio_tx_unit_enable → ESP_FAIL
    ParlioTransmit,  // parlio_tx_unit_transmit → ESP_FAIL
    MdnsInit,        // mdns_init → ESP_FAIL
    LcdNewPanel,     // esp_lcd_new_rgb_panel / _st7789 → ESP_FAIL
    LcdPanelReset,   // esp_lcd_panel_reset → ESP_FAIL
    LcdPanelInit,    // esp_lcd_panel_init → ESP_FAIL
    LcdFrameBuffer,  // esp_lcd_rgb_panel_get_frame_buffer → ESP_FAIL
    LcdDraw,         // esp_lcd_panel_draw_bitmap → ESP_FAIL
    LcdRefresh,      // esp_lcd_rgb_panel_refresh → ESP_FAIL
    LcdNewIo,        // esp_lcd_new_panel_io_spi → ESP_FAIL
    LcdTxColor,      // esp_lcd_panel_io_tx_color → ESP_FAIL (transfer refused)
    LcdTxLost,       // esp_lcd_panel_io_tx_color accepted, completion never fires
    SpiBus,          // spi_bus_initialize → ESP_FAIL
    CacheMsync,      // esp_cache_msync → ESP_FAIL
    I2cBus,          // i2c_new_master_bus → ESP_FAIL
    I2cAddDevice,    // i2c_master_bus_add_device → ESP_FAIL
    I2cTransmit,     // i2c_master_transmit → ESP_FAIL
    I2cReceive,      // i2c_master_receive → ESP_FAIL
    QueueCreate,     // xQueueCreate → nullptr
    I2sNew,          // i2s_new_channel → ESP_FAIL
    I2sWrite,        // i2s_channel_write → ESP_FAIL
    LedcTimer,       // ledc_timer_config → ESP_FAIL
    LedcChannel,     // ledc_channel_config → ESP_FAIL
    LedcFade,        // ledc_fade_func_install → ESP_FAIL
    NetifNew,        // esp_netif_new → nullptr
    EthMac,          // esp_eth_mac_new_esp32 → nullptr
    EthPhy,          // esp_eth_phy_new_ip101 → nullptr
    EthInstall,      // esp_eth_driver_install → ESP_FAIL
    EthStart,        // esp_eth_start → ESP_FAIL
    LdoAcquire,      // esp_ldo_acquire_channel → ESP_ERR_INVALID_STATE
    OtaDescription,  // esp_ota_get_partition_description → ESP_FAIL
    Count,
};
// `skip` calls succeed first (fail the Nth allocation, not the first).
void fail_next(Fault f, int count = 1, int skip = 0);
bool should_fail(Fault f);  // consumes one pending failure
void faults_clear();
void set_reset_reason(int reason);  // esp_reset_reason() value

// ── SD card (driver/sdmmc_host.h, esp_vfs_fat.h, vfs_redirect.h) ────────────
void sd_root(const std::string& host_dir);  // files of the card
void sd_insert(bool inserted);              // mount succeeds / status OK only when in

// ── LED output hardware (driver/parlio_tx.h, driver/gpio.h) ─────────────────
struct ParlioLog {
    int units_created = 0, units_deleted = 0, transmits = 0;
    size_t max_transfer_size = 0;
    uint32_t clk_hz          = 0;
    int data_gpios[16]       = {};  // the bus pin map of the last unit
    bool loop                = false;
    const void* last_buffer  = nullptr;
    std::vector<uint16_t> last_samples;  // the frame of the last transmit
};
ParlioLog& parlio_log();
void parlio_reset();
int gpio_calls();
unsigned gpio_level(int pin);              // last level driven, 2 if never driven
void gpio_input(int pin, unsigned level);  // what gpio_get_level() reads (default 1)
void psram_present(bool present);          // heap_caps_get_total_size(SPIRAM)
void psram_largest_block(size_t bytes);    // heap_caps_get_largest_free_block (0 = default)

// ── LCD (esp_lcd_panel_rgb.h, esp_lcd_panel_io.h, esp_lcd_panel_vendor.h) ───
struct LcdLog {
    int panels_created = 0, panels_deleted = 0, draws = 0, refreshes = 0, vsyncs = 0;
    uint32_t pclk_hz = 0, h_res = 0, v_res = 0;  // RGB panel geometry
    int data_gpios[16]     = {};
    const void* last_drawn = nullptr;  // buffer of the last draw_bitmap
    int last_x1 = 0, last_y1 = 0, last_x2 = 0, last_y2 = 0;
    std::vector<uint16_t> last_frame;  // RGB: the frame of the last refresh
    // SPI panel IO / vendor panels
    int ios_created = 0, spi_buses = 0, color_tx = 0;
    std::vector<int> commands;  // every tx_param / tx_color command, in order
    bool display_on = false, swap_xy = false, mirror_x = false, mirror_y = false;
    bool inverted = false;
    // SPI panel GRAM (CASET 0x2A / RASET 0x2B / RAMWR 0x2C), kGramW × kGramH.
    static constexpr int kGramW = 256, kGramH = 512;
    int win_x0 = 0, win_x1 = 0, win_y0 = 0, win_y1 = 0;
    size_t ram_pos = 0;  // pixels written since the last RAMWR (lcd_cmd -1 continues)
    // Colour bytes sent from a buffer the SPI driver would have to copy (addr
    // or length off the 64-byte line): what used to exhaust internal RAM.
    size_t bounced_bytes       = 0;
    std::vector<uint16_t> gram = std::vector<uint16_t>(kGramW * kGramH, 0xA5A5);
    uint16_t at(int x, int y) const { return gram[static_cast<size_t>(y) * kGramW + x]; }
};
LcdLog& lcd_log();
void lcd_reset();
// false: refresh never raises on_vsync (an emission that never completes).
void lcd_auto_vsync(bool on);

// ── I2C (driver/i2c_master.h) — a device model per address ─────────────────
struct I2cDevice {
    virtual ~I2cDevice()                                = default;
    virtual bool write(const uint8_t* data, size_t len) = 0;  // false = NACK
    virtual bool read(uint8_t* data, size_t len)        = 0;
};
void i2c_attach(uint16_t addr, I2cDevice* dev);  // nullptr detaches (address NACKs)
void i2c_nack_probes(int n);                     // the next n probes NACK anyway

// ── I2S (driver/i2s_std.h) ──────────────────────────────────────────────────
struct I2sLog {
    int channels = 0, deleted = 0;
    bool enabled = false, auto_clear = false;
    uint32_t sample_rate = 0, mclk_multiple = 0;
    int mclk = -1, bclk = -1, ws = -1, dout = -1;
    std::vector<int16_t> samples;                          // interleaved L/R, everything written
    void (*on_write)(size_t writes, void* ctx) = nullptr;  // after each write (writes so far)
    void* on_write_ctx                         = nullptr;
    size_t writes                              = 0;
};
I2sLog& i2s_log();
void i2s_reset();

// ── LEDC (driver/ledc.h) ────────────────────────────────────────────────────
struct LedcLog {
    int gpio         = -1;
    uint32_t freq_hz = 0, res_bits = 0;
    bool fade_installed = false;
    uint32_t duty = 0, pending_duty = 0, last_fade_ms = 0;
    int fades = 0, fade_stops = 0, steps = 0;
};
LedcLog& ledc_log();
void ledc_reset();

// ── Boot (esp_eth, esp_netif, esp_event, LDO, task WDT, OTA state) ─────────
struct NetifLog {
    bool created = false, dhcp_stopped = false, eth_started = false;
    uint32_t ip = 0, mask = 0, gw = 0;  // network order, as set_ip_info got them
    int handlers      = 0;              // esp_event_handler_register calls
    int dhcp_restarts = 0;              // esp_netif_dhcpc_start calls
    int mdc = -1, mdio = -1, phy_addr = -2, phy_reset = -2;
    int ldo_chan = -1, ldo_mv = 0;
    int all_multicast = -1;  // last ETH_CMD_S_ALL_MULTICAST (-1 = never set)
};
NetifLog& netif_log();
void boot_reset();
// Runs the handlers registered for (base, id) synchronously.
void event_post(const char* base, int32_t id, void* data = nullptr);
int wdt_resets();  // esp_task_wdt_reset() calls — one per render frame
// The partition esp_ota_get_last_invalid_partition() reports (nullptr: none)
// and its descriptor: `version`, and an ELF SHA of sha_seed, sha_seed+1, …
void ota_invalid_partition(const char* label, const char* version = "", uint8_t sha_seed = 0);
void ota_pending_verify(bool pending);  // running image state
bool ota_marked_valid();                // esp_ota_mark_app_valid_cancel_rollback() called

// ── OTA / core dump / mDNS ──────────────────────────────────────────────────
std::vector<uint8_t>& ota_image();                     // bytes esp_ota_write() received
bool ota_boot_switched();                              // esp_ota_set_boot_partition() called
void ota_fail_validation(bool fail);                   // esp_ota_end() rejects the image
void coredump_set(const std::vector<uint8_t>& image);  // empty = none
// `mac` (12 hex) and `hub` ("1"/"0") are the election TXT items; nullptr
// leaves them out, as an older firmware would.
void mdns_add_peer(const char* instance, uint32_t ip, const char* product, const char* node,
                   const char* fw, const char* mac = nullptr, const char* hub = nullptr,
                   uint16_t port = 80);
void set_mac(const uint8_t mac[6]);  // what esp_read_mac answers (default 30:ed:a0:12:34:56)
void mdns_reset();                   // forget the registered peers
std::string mdns_hostname();         // last mdns_hostname_set
uint32_t mdns_delegate_ip(const char* hostname);  // host order, 0 = not published
std::string mdns_txt(const char* key);            // the _http._tcp TXT item ("" = none)

}  // namespace shim
