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
};

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
    g_routes.clear();
    g_running = false;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* u) {
    if (g_routes.size() >= g_max_routes) return ESP_ERR_NO_MEM;  // as esp_http_server
    g_routes.push_back({ u->uri, u->method, u->handler, u->user_ctx });
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

namespace shim {

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
        HttpResponse r           = http_request(method.c_str(), target, body, hdrs);
        std::string out          = "HTTP/1.1 " +
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
