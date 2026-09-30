// Single-threaded FreeRTOS subset: the harness drives tasks explicitly and
// blocking calls advance the fake clock instead of sleeping.
#pragma once
#include <cstdint>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t EventBits_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffu
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1
typedef struct shim_sem* SemaphoreHandle_t;
typedef struct shim_eg* EventGroupHandle_t;
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
typedef struct {
    int unused;
} portMUX_TYPE;
// clang-format 18.1.3 (CI) and 18.1.8 disagree on brace-init macros.
// clang-format off
#define portMUX_INITIALIZER_UNLOCKED { 0 }
// clang-format on
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
