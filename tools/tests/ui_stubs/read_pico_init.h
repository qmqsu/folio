#pragma once
#include <stdbool.h>
#include "epdiy.h"
#include "esp_err.h"
typedef struct { bool touched; unsigned count; int x, y; } cst836u_touch_t;
typedef struct { EpdiyHighlevelState hl; uint8_t* framebuffer; void* touch; bool touch_ready, pmu_ready; } read_pico_handle_t;
esp_err_t read_pico_init(read_pico_handle_t* hardware);
esp_err_t cst836u_read(void* touch, cst836u_touch_t* sample);
