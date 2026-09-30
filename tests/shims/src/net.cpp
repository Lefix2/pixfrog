// In-process datagram fabric + task registry for the harness.
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <string>

#include "esp_mac.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "shim_control.h"

namespace {

struct Sock {
    uint16_t port = 0;
    std::vector<uint32_t> groups;
};
std::map<int, Sock> g_socks;
int g_next_fd = 100;
std::map<uint16_t, std::deque<shim::Datagram>> g_queues;
std::map<uint16_t, void (*)()> g_idle;
std::vector<shim::Datagram> g_sent;

struct Task {
    TaskFunction_t fn;
    void* arg;
};
std::map<std::string, Task> g_tasks;

}  // namespace

namespace shim {
void net_push(uint16_t port, const std::vector<uint8_t>& bytes, uint32_t from_ip) {
    g_queues[port].push_back({ from_ip, 0, bytes });
}
void net_on_idle(uint16_t port, void (*fn)()) {
    g_idle[port] = fn;
}
std::vector<Datagram>& net_sent() {
    return g_sent;
}
std::vector<uint32_t> net_groups(uint16_t port) {
    for (auto& [fd, s] : g_socks)
        if (s.port == port) return s.groups;
    return {};
}
void net_reset() {
    g_socks.clear();
    g_queues.clear();
    g_idle.clear();
    g_sent.clear();
}
bool run_task(const char* name) {
    auto it = g_tasks.find(name);
    if (it == g_tasks.end()) return false;
    const Task t = it->second;
    g_tasks.erase(it);  // a task body runs once; start() may register it again
    t.fn(t.arg);
    return true;
}
bool task_created(const char* name) {
    return g_tasks.count(name) != 0;
}
void tasks_forget() {
    g_tasks.clear();
}
}  // namespace shim

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t, void* arg,
                                   UBaseType_t, TaskHandle_t* out, BaseType_t) {
    static int handle;
    g_tasks[name] = { fn, arg };
    if (out) *out = &handle;
    return pdPASS;
}

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t) {
    static const uint8_t kMac[6] = { 0x30, 0xED, 0xA0, 0x12, 0x34, 0x56 };
    std::memcpy(mac, kMac, 6);
    return ESP_OK;
}

int shim_socket(int, int, int) {
    const int fd = g_next_fd++;
    g_socks[fd]  = Sock{};
    return fd;
}
int shim_bind(int fd, const sockaddr* addr, socklen_t) {
    auto it = g_socks.find(fd);
    if (it == g_socks.end()) return -1;
    it->second.port = ntohs(reinterpret_cast<const sockaddr_in*>(addr)->sin_port);
    return 0;
}
int shim_setsockopt(int fd, int level, int opt, const void* val, socklen_t) {
    auto it = g_socks.find(fd);
    if (it == g_socks.end()) return -1;
    if (level == IPPROTO_IP && (opt == IP_ADD_MEMBERSHIP || opt == IP_DROP_MEMBERSHIP)) {
        const uint32_t grp = ntohl(static_cast<const ip_mreq*>(val)->imr_multiaddr.s_addr);
        auto& gs           = it->second.groups;
        if (opt == IP_ADD_MEMBERSHIP)
            gs.push_back(grp);
        else
            gs.erase(std::remove(gs.begin(), gs.end(), grp), gs.end());
    }
    return 0;
}
ssize_t shim_recvfrom(int fd, void* buf, size_t len, int, sockaddr* from, socklen_t* fromlen) {
    auto it = g_socks.find(fd);
    if (it == g_socks.end()) return -1;
    auto& q = g_queues[it->second.port];
    if (q.empty()) {
        auto idle = g_idle.find(it->second.port);
        if (idle != g_idle.end() && idle->second) idle->second();
        return -1;
    }
    const auto d = q.front();
    q.pop_front();
    const size_t n = std::min(len, d.bytes.size());
    std::memcpy(buf, d.bytes.data(), n);
    if (from && fromlen && *fromlen >= sizeof(sockaddr_in)) {
        auto* sin            = reinterpret_cast<sockaddr_in*>(from);
        sin->sin_family      = AF_INET;
        sin->sin_addr.s_addr = htonl(d.ip);
        sin->sin_port        = htons(6454);
    }
    return static_cast<ssize_t>(n);
}
ssize_t shim_sendto(int, const void* buf, size_t len, int, const sockaddr* to, socklen_t) {
    const auto* sin = reinterpret_cast<const sockaddr_in*>(to);
    const auto* p   = static_cast<const uint8_t*>(buf);
    g_sent.push_back(
        { ntohl(sin->sin_addr.s_addr), ntohs(sin->sin_port), std::vector<uint8_t>(p, p + len) });
    return static_cast<ssize_t>(len);
}
int shim_shutdown(int, int) {
    return 0;
}
int shim_close(int fd) {
    g_socks.erase(fd);
    return 0;
}
