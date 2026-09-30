// Silent unless PIXFROG_TEST_LOG is set in the environment.
#pragma once
#include <cstdarg>
namespace shim {
void log_write(char level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
}
#define ESP_LOGE(tag, ...) ::shim::log_write('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ::shim::log_write('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ::shim::log_write('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ::shim::log_write('D', tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) ::shim::log_write('V', tag, __VA_ARGS__)
typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;
void esp_log_level_set(const char* tag, esp_log_level_t level);
typedef int (*vprintf_like_t)(const char*, va_list);
esp_log_level_t esp_log_level_get(const char* tag);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
