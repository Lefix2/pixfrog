#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t* previous_wake, TickType_t increment);
TickType_t xTaskGetTickCount();
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                                   UBaseType_t prio, TaskHandle_t* out, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                       UBaseType_t prio, TaskHandle_t* out);
void vTaskDelete(TaskHandle_t t);
