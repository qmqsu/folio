#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t read_pico_ioe_sleep(bool rails_off);
esp_err_t read_pico_ioe_wake(void);
void read_pico_epd_settle(void);

#define READ_PICO_IOE_INT_GPIO 41
void read_pico_clear_ioe_int(void);
