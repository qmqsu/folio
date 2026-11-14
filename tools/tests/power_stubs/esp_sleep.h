#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_SLEEP_WAKEUP_GPIO 1
#define ESP_SLEEP_WAKEUP_TIMER 2
esp_err_t esp_sleep_enable_gpio_wakeup(void);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us);
esp_err_t esp_sleep_disable_wakeup_source(int source);
esp_err_t esp_light_sleep_start(void);
