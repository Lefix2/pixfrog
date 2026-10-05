// esp_http_server on the host: an in-process dispatcher for contract tests,
// plus a minimal HTTP/1.1 listener (one request per connection) so a real
// browser can drive the same handlers.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "esp_http_server.h"
#include "shim_control.h"

namespace {

struct Route {
    std::string uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
    void* user_ctx;
    bool websocket;
};

// WebSocket clients: in-process ones (sock -1) keep what was pushed to them;
// served ones (a real browser) get real frames on their socket.
struct WsClient {
    int sock = -1;
    std::vector<std::string> frames;
    bool stalled = false;  // shim::ws_stall: every send to it fails
};
bool g_hold_work = false;
std::vector<std::pair<httpd_work_fn_t, void*>> g_held_work;
std::map<int, WsClient> g_ws;
int g_next_ws_fd = 100;
// httpd_queue_work runs on the caller's thread; in serve mode that is another
// thread than the request loop, so both take this lock.
std::recursive_mutex g_serve_mux;

struct Ctx {  // per-request state behind httpd_req_t::aux
    std::string body;
    size_t read = 0;
    std::map<std::string, std::string> req_headers;  // lower-cased names
    shim::HttpResponse resp;
};

std::vector<Route> g_routes;
bool g_running                 = false;
size_t g_max_routes            = 8;
httpd_uri_match_func_t g_match = nullptr;
size_t g_chunk                 = 0;

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Ctx& ctx(httpd_req_t* r) {
    return *static_cast<Ctx*>(r->aux);
}

httpd_method_t method_of(const char* m) {
    if (!std::strcmp(m, "GET")) return HTTP_GET;
    if (!std::strcmp(m, "POST")) return HTTP_POST;
    if (!std::strcmp(m, "DELETE")) return HTTP_DELETE;
    if (!std::strcmp(m, "PUT")) return HTTP_PUT;
    return HTTP_HEAD;
}

}  // namespace

bool httpd_uri_match_wildcard(const char* tmpl, const char* uri, size_t len) {
    const size_t tl = std::strlen(tmpl);
    if (tl && tmpl[tl - 1] == '*') return len >= tl - 1 && std::strncmp(tmpl, uri, tl - 1) == 0;
    return tl == len && std::strncmp(tmpl, uri, len) == 0;
}

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* cfg) {
    if (shim::should_fail(shim::Fault::HttpdStart)) return ESP_FAIL;
    static int h;
    g_routes.clear();
    g_match      = cfg->uri_match_fn;
    g_max_routes = cfg->max_uri_handlers;
    g_running    = true;
    *handle      = &h;
    return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
        for (auto& [fd, c] : g_ws)
            if (c.sock >= 0) ::close(c.sock);
        g_ws.clear();
    }
    g_routes.clear();
    g_running = false;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* u) {
    if (g_routes.size() >= g_max_routes) return ESP_ERR_NO_MEM;  // as esp_http_server
    g_routes.push_back({ u->uri, u->method, u->handler, u->user_ctx, u->is_websocket });
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t* r, char* buf, size_t len) {
    Ctx& c   = ctx(r);
    size_t n = std::min(len, c.body.size() - c.read);
    if (g_chunk) n = std::min(n, g_chunk);
    if (n == 0) return 0;
    std::memcpy(buf, c.body.data() + c.read, n);
    c.read += n;
    return static_cast<int>(n);
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* field, char* val, size_t len) {
    auto& h = ctx(r).req_headers;
    auto it = h.find(lower(field));
    if (it == h.end()) return ESP_ERR_NOT_FOUND;
    std::snprintf(val, len, "%s", it->second.c_str());
    return it->second.size() >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t* r, const char* field) {
    auto& h = ctx(r).req_headers;
    auto it = h.find(lower(field));
    return it == h.end() ? 0 : it->second.size();
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t len) {
    const char* q = std::strchr(r->uri, '?');
    if (!q) return ESP_ERR_NOT_FOUND;
    std::snprintf(buf, len, "%s", q + 1);
    return std::strlen(q + 1) >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}
esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t len) {
    const size_t kl = std::strlen(key);
    for (const char* p = qry; p && *p;) {
        const char* amp = std::strchr(p, '&');
        const char* end = amp ? amp : p + std::strlen(p);
        if (static_cast<size_t>(end - p) > kl && !std::strncmp(p, key, kl) && p[kl] == '=') {
            const size_t vl = static_cast<size_t>(end - p) - kl - 1;
            std::snprintf(val, len, "%.*s", static_cast<int>(vl), p + kl + 1);
            return vl >= len ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
        }
        p = amp ? amp + 1 : nullptr;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status) {
    ctx(r).resp.status_line = status;
    ctx(r).resp.status      = std::atoi(status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type) {
    ctx(r).resp.content_type = type;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* field, const char* value) {
    ctx(r).resp.headers[field] = value;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len) {
    if (buf && len) ctx(r).resp.body.append(buf, len < 0 ? std::strlen(buf) : size_t(len));
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t* r, const char* buf, ssize_t len) {
    return httpd_resp_send(r, buf, len);
}
esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* str) {
    return httpd_resp_send(r, str, HTTPD_RESP_USE_STRLEN);
}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t* r, const char* str) {
    return str ? httpd_resp_send(r, str, HTTPD_RESP_USE_STRLEN) : ESP_OK;
}

esp_err_t httpd_resp_send_500(httpd_req_t* r) {
    httpd_resp_set_status(r, "500 Internal Server Error");
    return httpd_resp_sendstr(r, "Internal Server Error");
}

namespace {

// Server → client frame (unmasked), RFC 6455 §5.2.
std::string ws_frame(uint8_t opcode, const std::string& payload) {
    std::string f(1, static_cast<char>(0x80 | opcode));
    const size_t n = payload.size();
    if (n < 126) {
        f += static_cast<char>(n);
    } else if (n < 65536) {
        f += static_cast<char>(126);
        f += static_cast<char>(n >> 8);
        f += static_cast<char>(n & 0xFF);
    } else {
        f += static_cast<char>(127);
        for (int i = 7; i >= 0; --i)
            f += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF);
    }
    return f + payload;
}

const Route* ws_route(const std::string& path) {
    for (const auto& r : g_routes)
        if (r.websocket && r.method == HTTP_GET &&
            (g_match ? g_match(r.uri.c_str(), path.c_str(), path.size()) : r.uri == path))
            return &r;
    return nullptr;
}

// The handshake reaches the handler as a GET, as in esp_http_server.
int ws_register(const Route& route, const std::string& path, int sock) {
    Ctx c;
    httpd_req_t r{};
    r.method = HTTP_GET;
    std::snprintf(r.uri, sizeof(r.uri), "%s", path.c_str());
    r.aux      = &c;
    r.user_ctx = route.user_ctx;
    route.handler(&r);
    const int fd  = g_next_ws_fd++;
    g_ws[fd].sock = sock;
    return fd;
}

// SHA-1 (FIPS 180-1), only for the WebSocket handshake of the test server.
std::string sha1(const std::string& msg) {
    uint32_t h[5]  = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::string m  = msg;
    m             += static_cast<char>(0x80);
    while (m.size() % 64 != 56)
        m += '\0';
    const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
    for (int i = 7; i >= 0; --i)
        m += static_cast<char>((bits >> (8 * i)) & 0xFF);
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(m[off + 4 * i])) << 24) |
                   (uint32_t(uint8_t(m[off + 4 * i + 1])) << 16) |
                   (uint32_t(uint8_t(m[off + 4 * i + 2])) << 8) |
                   uint32_t(uint8_t(m[off + 4 * i + 3]));
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e                = d;
            d                = c;
            c                = rol(b, 30);
            b                = a;
            a                = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i)
            out += static_cast<char>((v >> (8 * i)) & 0xFF);
    return out;
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) |
                           uint8_t(in[i + 2]);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace

esp_err_t httpd_ws_recv_frame(httpd_req_t*, httpd_ws_frame_t* pkt, size_t) {
    pkt->len = 0;  // clients send nothing the firmware reads
    return ESP_OK;
}
esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int fd, httpd_ws_frame_t* frame) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    auto it = g_ws.find(fd);
    if (it == g_ws.end() || it->second.stalled) return ESP_FAIL;
    const std::string payload(reinterpret_cast<const char*>(frame->payload), frame->len);
    if (it->second.sock < 0) {
        it->second.frames.push_back(payload);
        return ESP_OK;
    }
    const std::string f = ws_frame(static_cast<uint8_t>(frame->type), payload);
    if (::send(it->second.sock, f.data(), f.size(), MSG_NOSIGNAL) != ssize_t(f.size())) {
        ::close(it->second.sock);  // the browser went away
        g_ws.erase(it);
        return ESP_FAIL;
    }
    return ESP_OK;
}
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t, int fd) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    return g_ws.count(fd) ? HTTPD_WS_CLIENT_WEBSOCKET : HTTPD_WS_CLIENT_INVALID;
}
esp_err_t httpd_get_client_list(httpd_handle_t, size_t* fds, int* client_fds) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    size_t n = 0;
    for (auto& [fd, c] : g_ws)
        if (n < *fds) client_fds[n++] = fd;
    *fds = n;
    return ESP_OK;
}
esp_err_t httpd_queue_work(httpd_handle_t, httpd_work_fn_t work, void* arg) {
    if (!g_running) return ESP_FAIL;
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    if (g_hold_work)
        g_held_work.emplace_back(work, arg);
    else
        work(arg);
    return ESP_OK;
}
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int sockfd) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    auto it = g_ws.find(sockfd);
    if (it == g_ws.end()) return ESP_FAIL;
    if (it->second.sock >= 0) ::close(it->second.sock);
    g_ws.erase(it);
    return ESP_OK;
}

namespace shim {

int ws_open(const std::string& uri) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    const Route* r = ws_route(uri);
    return r ? ws_register(*r, uri, -1) : -1;
}
std::vector<std::string> ws_frames(int fd) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    auto it = g_ws.find(fd);
    return it == g_ws.end() ? std::vector<std::string>{} : it->second.frames;
}
void ws_close(int fd) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    g_ws.erase(fd);
}
void ws_stall(int fd) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    auto it = g_ws.find(fd);
    if (it != g_ws.end()) it->second.stalled = true;
}
bool ws_is_open(int fd) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    return g_ws.count(fd) != 0;
}
void http_hold_work(bool hold) {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    g_hold_work = hold;
    if (hold) return;
    const auto pending = std::move(g_held_work);
    g_held_work.clear();
    for (const auto& w : pending)
        w.first(w.second);
}
size_t http_pending_work() {
    std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
    return g_held_work.size();
}

size_t http_routes() {
    return g_routes.size();
}
void http_recv_chunk(size_t max) {
    g_chunk = max;
}
bool http_running() {
    return g_running;
}

HttpResponse http_request(const char* method, const std::string& uri, const std::string& body,
                          const std::map<std::string, std::string>& headers) {
    Ctx c;
    c.body = body;
    for (auto& [k, v] : headers)
        c.req_headers[lower(k)] = v;
    httpd_req_t r{};
    r.method      = method_of(method);
    r.content_len = body.size();
    std::snprintf(r.uri, sizeof(r.uri), "%s", uri.c_str());
    r.aux                 = &c;
    const size_t path_len = uri.find('?') == std::string::npos ? uri.size() : uri.find('?');
    for (const auto& route : g_routes) {
        if (route.method != r.method) continue;
        const bool hit = g_match ? g_match(route.uri.c_str(), r.uri, path_len)
                                 : route.uri.compare(0, std::string::npos, uri, 0, path_len) == 0;
        if (!hit) continue;
        r.user_ctx     = route.user_ctx;
        c.resp.handled = true;
        route.handler(&r);
        break;
    }
    if (!c.resp.handled) c.resp.status = 404;
    if (c.resp.status == 0) c.resp.status = 200;
    return c.resp;
}

void http_serve(uint16_t port, volatile bool* stop) {
    const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes      = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(port);
    if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 || ::listen(ls, 16) < 0) {
        std::perror("http_serve");
        return;
    }
    timeval tv{ 0, 200000 };
    ::setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!*stop) {
        const int cs = ::accept(ls, nullptr, nullptr);
        if (cs < 0) continue;
        std::string in;
        char buf[4096];
        size_t hdr_end = std::string::npos;
        while (hdr_end == std::string::npos) {
            const ssize_t n = ::recv(cs, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, size_t(n));
            hdr_end = in.find("\r\n\r\n");
        }
        if (hdr_end == std::string::npos) {
            ::close(cs);
            continue;
        }
        std::map<std::string, std::string> hdrs;
        size_t pos           = in.find("\r\n");
        const std::string rl = in.substr(0, pos);
        while (pos < hdr_end) {
            const size_t next   = in.find("\r\n", pos + 2);
            const std::string l = in.substr(pos + 2, next - pos - 2);
            const size_t colon  = l.find(':');
            if (colon != std::string::npos) {
                std::string v = l.substr(colon + 1);
                v.erase(0, v.find_first_not_of(' '));
                hdrs[lower(l.substr(0, colon))] = v;
            }
            pos = next;
        }
        const size_t want = hdrs.count("content-length") ? std::stoul(hdrs["content-length"]) : 0;
        std::string body  = in.substr(hdr_end + 4);
        while (body.size() < want) {
            const ssize_t n = ::recv(cs, buf, sizeof(buf), 0);
            if (n <= 0) break;
            body.append(buf, size_t(n));
        }
        const size_t sp1 = rl.find(' '), sp2 = rl.find(' ', sp1 + 1);
        const std::string method = rl.substr(0, sp1);
        const std::string target = rl.substr(sp1 + 1, sp2 - sp1 - 1);
        std::lock_guard<std::recursive_mutex> lock(g_serve_mux);
        // WebSocket upgrade (RFC 6455 handshake): the socket stays open and
        // joins the client list the firmware pushes to.
        if (lower(hdrs["upgrade"]) == "websocket" && hdrs.count("sec-websocket-key")) {
            const Route* wr = ws_route(target);
            if (wr) {
                const std::string accept = base64(
                    sha1(hdrs["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
                const std::string resp =
                    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                    "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                    accept + "\r\n\r\n";
                ::send(cs, resp.data(), resp.size(), MSG_NOSIGNAL);
                ws_register(*wr, target, cs);
                continue;
            }
        }
        HttpResponse r  = http_request(method.c_str(), target, body, hdrs);
        std::string out = "HTTP/1.1 " +
                          (r.status_line.empty() ? std::to_string(r.status) + " OK"
                                                 : r.status_line) +
                          "\r\n";
        if (!r.content_type.empty()) out += "Content-Type: " + r.content_type + "\r\n";
        for (auto& [k, v] : r.headers)
            out += k + ": " + v + "\r\n";
        out += "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n";
        out += r.body;
        for (size_t off = 0; off < out.size();) {
            const ssize_t n = ::send(cs, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (n <= 0) break;
            off += size_t(n);
        }
        ::close(cs);
    }
    ::close(ls);
}

}  // namespace shim
