// Host shim: the task watchdog. A reset is the render loop's once-per-frame
// heartbeat, so it is also where shim::run_task_for counts a frame.
#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_reset();
