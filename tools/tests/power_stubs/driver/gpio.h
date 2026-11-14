#pragma once
#include <stdint.h>
#include "esp_err.h"
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_LOW_LEVEL 1
typedef struct {uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t* c);
esp_err_t gpio_wakeup_enable(int pin,int level);
esp_err_t gpio_wakeup_disable(int pin);
