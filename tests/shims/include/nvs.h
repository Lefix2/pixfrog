#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* data, size_t len);
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key);
