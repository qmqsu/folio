#pragma once
#include <stdbool.h>
#include "esp_err.h"

static inline esp_err_t sleep_cpu_configure(bool enabled) {
    (void)enabled;
    return ESP_OK;
}
