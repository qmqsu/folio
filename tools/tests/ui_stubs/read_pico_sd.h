#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    bool mounted, present, needs_format;
    uint64_t free_bytes, capacity_bytes;
    esp_err_t error;
} read_pico_sd_info_t;
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info);
esp_err_t read_pico_sd_remount(void);
esp_err_t read_pico_sd_sync(void);
esp_err_t read_pico_sd_format(void);
