#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { unsigned tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
static inline bool usb_serial_jtag_is_driver_installed(void) { return true; }
static inline esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t* config) {
    (void)config; return ESP_OK;
}
static inline int usb_serial_jtag_read_bytes(void* data, size_t size, unsigned ticks) {
    (void)data; (void)size; (void)ticks; return 0;
}
