#pragma once
#include <stddef.h>
typedef void (*TaskFunction_t)(void*);
int xTaskCreate(TaskFunction_t function, const char* name, unsigned stack, void* arg, unsigned priority, void* handle);
void vTaskDelay(unsigned ticks);
void vTaskDelete(void* task);
static inline void* xTaskGetCurrentTaskHandle(void) { return (void*)1; }
