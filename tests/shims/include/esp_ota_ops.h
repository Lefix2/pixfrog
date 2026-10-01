#pragma once
#include "esp_app_desc.h"
#include "esp_err.h"
#include <cstdint>
typedef struct {
    char label[17];
    uint32_t address;
    uint32_t size;
} esp_partition_t;
const esp_partition_t* esp_ota_get_running_partition();
typedef uint32_t esp_ota_handle_t;
#define OTA_SIZE_UNKNOWN 0xffffffff
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffe
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start);
esp_err_t esp_ota_begin(const esp_partition_t* part, size_t image_size, esp_ota_handle_t* out);
esp_err_t esp_ota_write(esp_ota_handle_t h, const void* data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t h);
esp_err_t esp_ota_abort(esp_ota_handle_t h);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* part);
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum {
    ESP_PARTITION_SUBTYPE_DATA_COREDUMP = 0x03,
    ESP_PARTITION_SUBTYPE_ANY           = 0xff
} esp_partition_subtype_t;
const esp_partition_t* esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype, const char* label);
esp_err_t esp_partition_read(const esp_partition_t* part, size_t off, void* dst, size_t size);
typedef enum {
    ESP_OTA_IMG_NEW            = 0,
    ESP_OTA_IMG_PENDING_VERIFY = 1,
    ESP_OTA_IMG_VALID          = 2,
    ESP_OTA_IMG_INVALID        = 3,
    ESP_OTA_IMG_ABORTED        = 4,
    ESP_OTA_IMG_UNDEFINED      = -1,
} esp_ota_img_states_t;
const esp_partition_t* esp_ota_get_last_invalid_partition();
esp_err_t esp_ota_get_partition_description(const esp_partition_t* part, esp_app_desc_t* desc);
esp_err_t esp_ota_get_state_partition(const esp_partition_t* part, esp_ota_img_states_t* state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
