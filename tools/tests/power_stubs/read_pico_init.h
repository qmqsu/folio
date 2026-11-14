#pragma once
#include <stdbool.h>
#include "esp_err.h"
#define CST836U_MODE_DEEPSLEEP 3
typedef struct { bool sensor_ready, touch_ready; void* sensor; void* touch; } read_pico_handle_t;
esp_err_t sc7a20h_power_down(void* sensor);
esp_err_t cst836u_set_mode(void* touch, int mode);
esp_err_t cst836u_wake(void* touch);

bool sc7a20h_powered(void* sensor);
esp_err_t sc7a20h_power_up(void* sensor);
