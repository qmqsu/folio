#pragma once
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
BaseType_t xTaskCreate(void (*entry)(void*),const char*,uint32_t,void*,unsigned,TaskHandle_t*);
