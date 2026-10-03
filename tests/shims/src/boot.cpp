// What only app_main() touches: Ethernet + netif + the default event loop, the
// VDD_IO_5 LDO, the task watchdog and the OTA image state. Recorded for the
// boot harness (shim::netif_log, shim::event_post, shim::ota_*).
#include <cstring>
#include <string>
#include <vector>

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_ldo_regulator.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "shim_control.h"

const esp_event_base_t IP_EVENT  = "IP_EVENT";
const esp_event_base_t ETH_EVENT = "ETH_EVENT";

struct esp_netif_obj {
    int unused;
};
struct esp_eth_driver_t {
    int unused;
};
struct ldo_unit_ctx_t {
    int chan;
};

namespace {
struct Handler {
    std::string base;
    int32_t id;
    esp_event_handler_t fn;
    void* arg;
};
std::vector<Handler> g_handlers;
shim::NetifLog g_netif;
esp_netif_t g_the_netif{};
esp_eth_mac_t g_mac{};
esp_eth_phy_t g_phy{};
esp_eth_driver_t g_eth{};
ldo_unit_ctx_t g_ldo{};
int g_wdt_resets = 0;

esp_partition_t g_invalid{};
esp_app_desc_t g_invalid_desc{};
bool g_has_invalid  = false;
bool g_pending      = false;
bool g_marked_valid = false;
}  // namespace

namespace shim {
void event_post(const char* base, int32_t id, void* data) {
    const auto handlers = g_handlers;  // a handler may register more
    for (const auto& h : handlers)
        if (h.base == base && (h.id == ESP_EVENT_ANY_ID || h.id == id))
            h.fn(h.arg, h.base == IP_EVENT ? IP_EVENT : ETH_EVENT, id, data);
}
NetifLog& netif_log() {
    return g_netif;
}
void boot_reset() {
    g_handlers.clear();
    g_netif        = NetifLog{};
    g_wdt_resets   = 0;
    g_has_invalid  = false;
    g_pending      = false;
    g_marked_valid = false;
}
int wdt_resets() {
    return g_wdt_resets;
}
void ota_invalid_partition(const char* label, const char* version, uint8_t sha_seed) {
    g_has_invalid = label != nullptr;
    if (!label) return;
    std::strncpy(g_invalid.label, label, sizeof(g_invalid.label) - 1);
    std::memset(&g_invalid_desc, 0, sizeof(g_invalid_desc));
    std::memcpy(g_invalid_desc.version, version, std::strlen(version));  // may fill all 32
    for (size_t i = 0; i < sizeof(g_invalid_desc.app_elf_sha256); ++i)
        g_invalid_desc.app_elf_sha256[i] = static_cast<uint8_t>(sha_seed + i);
}
void ota_pending_verify(bool pending) {
    g_pending = pending;
}
bool ota_marked_valid() {
    return g_marked_valid;
}
}  // namespace shim

// ── Event loop ──────────────────────────────────────────────────────────────

esp_err_t esp_event_loop_create_default() {
    return ESP_OK;
}
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler,
                                     void* arg) {
    g_handlers.push_back({ base, id, handler, arg });
    ++g_netif.handlers;
    return ESP_OK;
}

// ── Netif ───────────────────────────────────────────────────────────────────

esp_err_t esp_netif_init() {
    return ESP_OK;
}
esp_netif_t* esp_netif_new(const esp_netif_config_t*) {
    if (shim::should_fail(shim::Fault::NetifNew)) return nullptr;
    g_netif.created = true;
    return &g_the_netif;
}
esp_err_t esp_netif_attach(esp_netif_t* netif, esp_netif_iodriver_handle glue) {
    return netif && glue ? ESP_OK : ESP_ERR_INVALID_ARG;
}
esp_err_t esp_netif_dhcpc_stop(esp_netif_t*) {
    g_netif.dhcp_stopped = true;
    return ESP_OK;
}
esp_err_t esp_netif_dhcpc_start(esp_netif_t*) {
    g_netif.dhcp_stopped = false;
    ++g_netif.dhcp_restarts;
    return ESP_OK;
}
esp_err_t esp_netif_set_ip_info(esp_netif_t*, const esp_netif_ip_info_t* info) {
    g_netif.ip   = info->ip.addr;  // network order, as lwIP stores it
    g_netif.mask = info->netmask.addr;
    g_netif.gw   = info->gw.addr;
    return ESP_OK;
}

// ── Ethernet ────────────────────────────────────────────────────────────────

esp_eth_mac_t* esp_eth_mac_new_esp32(const eth_esp32_emac_config_t* esp32_cfg,
                                     const eth_mac_config_t*) {
    if (shim::should_fail(shim::Fault::EthMac)) return nullptr;
    g_netif.mdc  = esp32_cfg->smi_gpio.mdc_num;
    g_netif.mdio = esp32_cfg->smi_gpio.mdio_num;
    return &g_mac;
}
esp_eth_phy_t* esp_eth_phy_new_ip101(const eth_phy_config_t* cfg) {
    if (shim::should_fail(shim::Fault::EthPhy)) return nullptr;
    g_netif.phy_addr  = cfg->phy_addr;
    g_netif.phy_reset = cfg->reset_gpio_num;
    return &g_phy;
}
esp_err_t esp_eth_driver_install(const esp_eth_config_t* cfg, esp_eth_handle_t* out) {
    if (shim::should_fail(shim::Fault::EthInstall)) return ESP_FAIL;
    if (!cfg->mac || !cfg->phy) return ESP_ERR_INVALID_ARG;
    *out = &g_eth;
    return ESP_OK;
}
esp_netif_iodriver_handle esp_eth_new_netif_glue(esp_eth_handle_t eth) {
    return eth;
}
esp_err_t esp_eth_ioctl(esp_eth_handle_t eth, esp_eth_io_cmd_t cmd, void* data) {
    if (!eth || !data) return ESP_ERR_INVALID_ARG;
    if (cmd == ETH_CMD_S_ALL_MULTICAST) g_netif.all_multicast = *static_cast<bool*>(data) ? 1 : 0;
    return ESP_OK;
}
esp_err_t esp_eth_start(esp_eth_handle_t) {
    if (shim::should_fail(shim::Fault::EthStart)) return ESP_FAIL;
    g_netif.eth_started = true;
    return ESP_OK;
}

// ── LDO, watchdog ───────────────────────────────────────────────────────────

esp_err_t esp_ldo_acquire_channel(const esp_ldo_channel_config_t* cfg,
                                  esp_ldo_channel_handle_t* out) {
    if (shim::should_fail(shim::Fault::LdoAcquire)) return ESP_ERR_INVALID_STATE;
    g_netif.ldo_chan = cfg->chan_id;
    g_netif.ldo_mv   = cfg->voltage_mv;
    g_ldo.chan       = cfg->chan_id;
    *out             = &g_ldo;
    return ESP_OK;
}

namespace shim {
void task_delay_point();  // net.cpp: run_task_for's budget
}
esp_err_t esp_task_wdt_add(TaskHandle_t) {
    return ESP_OK;
}
esp_err_t esp_task_wdt_reset() {
    ++g_wdt_resets;
    shim::task_delay_point();
    return ESP_OK;
}

// ── OTA image state ─────────────────────────────────────────────────────────

const esp_partition_t* esp_ota_get_last_invalid_partition() {
    return g_has_invalid ? &g_invalid : nullptr;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t* part, esp_app_desc_t* desc) {
    if (shim::should_fail(shim::Fault::OtaDescription)) return ESP_FAIL;
    if (part != &g_invalid) return ESP_ERR_NOT_FOUND;
    *desc = g_invalid_desc;
    return ESP_OK;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* state) {
    *state = g_pending ? ESP_OTA_IMG_PENDING_VERIFY : ESP_OTA_IMG_VALID;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    g_marked_valid = true;
    g_pending      = false;
    return ESP_OK;
}
