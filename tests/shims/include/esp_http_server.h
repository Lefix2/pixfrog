// esp_http_server subset. Handlers register into a table; the harness
// dispatches requests to them in-process (shim::http_request) — or, in serve
// mode, from a real TCP listener for browser tests (shim::http_serve).
#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#include <sys/types.h>
typedef void* httpd_handle_t;
typedef enum {
    HTTP_DELETE = 0,
    HTTP_GET    = 1,
    HTTP_HEAD   = 2,
    HTTP_POST   = 3,
    HTTP_PUT    = 4
} httpd_method_t;
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_ERR_HTTPD_RESULT_TRUNC 0xb008
typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    char uri[512];
    size_t content_len;
    void* aux;
    void* user_ctx;
    void* sess_ctx;
} httpd_req_t;
typedef struct {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t* r);
    void* user_ctx;
} httpd_uri_t;
typedef bool (*httpd_uri_match_func_t)(const char* tmpl, const char* uri, size_t len);
typedef struct {
    unsigned task_priority;
    size_t stack_size;
    int core_id;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() { 5, 4096, 0x7fffffff, 80, 32768, 7, 8, 8, 5, false, 5, 5, nullptr }
bool httpd_uri_match_wildcard(const char* tmpl, const char* uri, size_t len);
esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* cfg);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* uri);
int httpd_req_recv(httpd_req_t* r, char* buf, size_t len);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* r, const char* field, char* val, size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t* r, const char* field);
esp_err_t httpd_req_get_url_query_str(httpd_req_t* r, char* buf, size_t len);
esp_err_t httpd_query_key_value(const char* qry, const char* key, char* val, size_t len);
esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status);
esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type);
esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* field, const char* value);
esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t* r, const char* buf, ssize_t len);
esp_err_t httpd_resp_sendstr(httpd_req_t* r, const char* str);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t* r, const char* str);
esp_err_t httpd_resp_send_500(httpd_req_t* r);
