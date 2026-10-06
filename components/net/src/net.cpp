#include "net.h"

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/inet.h"

namespace pixfrog {
namespace net {
namespace {

constexpr const char* TAG = "net";

esp_netif_t* g_netif = nullptr;
Publisher g_publish  = nullptr;
bool g_link_up       = false;

// Static mode: the configured address, republished on every link-up. lwIP
// raises IP_EVENT_ETH_GOT_IP for a DHCP lease only — esp_netif_up() skips the
// event when the address was set before the link — so nothing else would hand
// the static IP to the UI and Art-Net after a cable replug.
uint32_t g_static_ip = 0;

// DHCP mode: which fallback replaced a missing lease (see on_got_ip).
bool g_dhcp_mode              = false;
bool g_artnet_fallback_active = false;

// Single derivation point for the address the box advertises. A configured
// static IP is not an address while the cable is out, so the link gates it.
void publish(uint32_t host_order_ip) {
    if (!g_link_up) host_order_ip = 0;
    const State st = !g_link_up         ? State::Disconnected
                   : host_order_ip != 0 ? State::Connected
                                        : State::Acquiring;
    if (g_publish) g_publish(host_order_ip, st);
}

void log_ip(const char* what, uint32_t ip) {
    ESP_LOGI(TAG, "%s %u.%u.%u.%u", what, static_cast<unsigned>((ip >> 24) & 0xFFu),
             static_cast<unsigned>((ip >> 16) & 0xFFu), static_cast<unsigned>((ip >> 8) & 0xFFu),
             static_cast<unsigned>(ip & 0xFFu));
}

// The configured static address on the netif, DHCP client stopped.
void set_static(const config::GlobalConfig& g) {
    esp_netif_dhcpc_stop(g_netif);
    esp_netif_ip_info_t info{};
    info.ip.addr      = lwip_htonl(g.static_ip);
    info.netmask.addr = lwip_htonl(g.static_mask ? g.static_mask : 0xFFFFFF00u);
    info.gw.addr      = lwip_htonl(g.static_gateway);
    esp_netif_set_ip_info(g_netif, &info);
    g_static_ip = g.static_ip;
    g_dhcp_mode = false;
    log_ip("static IP", g.static_ip);
}

// No DHCP server answered and lwIP AutoIP took a link-local address, but the
// user chose the Art-Net convention: stop the DHCP client (AutoIP with it) and
// take 2.x.y.z/8 from the MAC instead. DHCP is retried at the next link-up.
void apply_artnet_fallback() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_ETH);
    const uint32_t ip = config::artnet_fallback_ip(mac);
    esp_netif_dhcpc_stop(g_netif);
    esp_netif_ip_info_t info{};
    info.ip.addr      = lwip_htonl(ip);
    info.netmask.addr = lwip_htonl(config::kArtnetFallbackMask);
    esp_netif_set_ip_info(g_netif, &info);
    g_artnet_fallback_active = true;
    publish(ip);
    ESP_LOGW(TAG, "no DHCP server: Art-Net fallback %u.%u.%u.%u/8", static_cast<unsigned>(ip >> 24),
             static_cast<unsigned>((ip >> 16) & 0xFFu), static_cast<unsigned>((ip >> 8) & 0xFFu),
             static_cast<unsigned>(ip & 0xFFu));
}

// IP_EVENT_ETH_GOT_IP — a DHCP lease, the link-local fallback lwIP takes
// without one, or a static address set while the link is up.
void on_got_ip(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* event_data) {
    auto* event            = static_cast<ip_event_got_ip_t*>(event_data);
    const uint32_t host_ip = lwip_ntohl(event->ip_info.ip.addr);
    if (g_dhcp_mode && config::is_link_local(host_ip) &&
        config::get_global().ip_fallback == config::kIpFallbackArtnet) {
        apply_artnet_fallback();
        return;
    }
    publish(host_ip);
    log_ip("GOT_IP", host_ip);
}

void on_eth_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t event_id,
                  void* /*event_data*/) {
    switch (event_id) {
    case ETHERNET_EVENT_START: ESP_LOGI(TAG, "Ethernet driver started"); break;
    case ETHERNET_EVENT_STOP: ESP_LOGI(TAG, "Ethernet driver stopped"); break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link UP");
        g_link_up = true;
        // A replugged cable may lead to a DHCP server now: ask again.
        if (g_artnet_fallback_active) {
            g_artnet_fallback_active = false;
            esp_netif_dhcpc_start(g_netif);
        }
        // Static mode: the address is usable the instant the link is. DHCP mode
        // passes 0, which lands on Acquiring until IP_EVENT_ETH_GOT_IP fires.
        publish(g_static_ip);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link DOWN");
        g_link_up = false;
        publish(0);
        break;
    default: break;
    }
}

}  // namespace

void init(esp_netif_t* netif, Publisher publisher) {
    g_netif   = netif;
    g_publish = publisher;
    // Static vs DHCP, ahead of link-up so there is no transient DHCP attempt.
    const auto& g = config::get_global();
    if (!g.use_dhcp && g.static_ip != 0) {
        set_static(g);
    } else {
        g_dhcp_mode = true;
        ESP_LOGI(TAG, "DHCP enabled, awaiting lease (fallback: %s)",
                 config::ip_fallback_id(g.ip_fallback));
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, nullptr);
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, nullptr);
}

void apply(const config::GlobalConfig& g) {
    if (!g_netif) return;
    g_artnet_fallback_active = false;
    if (!g.use_dhcp && g.static_ip != 0) {
        set_static(g);
        publish(g_static_ip);  // usable at once with the link up
    } else {
        // A fresh lease: the client restarted drops the address it had, so the
        // UI shows Acquiring until the server answers (or the fallback does).
        g_static_ip = 0;
        g_dhcp_mode = true;
        esp_netif_dhcpc_stop(g_netif);
        esp_netif_dhcpc_start(g_netif);
        publish(0);
        ESP_LOGI(TAG, "DHCP enabled, awaiting lease (fallback: %s)",
                 config::ip_fallback_id(g.ip_fallback));
    }
}

bool link_up() {
    return g_link_up;
}

}  // namespace net
}  // namespace pixfrog
