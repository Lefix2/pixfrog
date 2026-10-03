// Host shim: FreeRTOS queues (copy-in / copy-out, FIFO). An empty receive is a
// blocking point like vTaskDelay: the clock moves by its timeout (1 s for
// portMAX_DELAY) and run_task_for counts it.
#pragma once
#include "FreeRTOS.h"
#include "task.h"
typedef struct QueueDefinition* QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t ticks);
BaseType_t xQueueOverwrite(QueueHandle_t q, const void* item);  // length-1 mailbox
BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);
