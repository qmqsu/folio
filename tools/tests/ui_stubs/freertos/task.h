#pragma once
static inline void* xTaskGetCurrentTaskHandle(void) {return (void*)1;}
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }
