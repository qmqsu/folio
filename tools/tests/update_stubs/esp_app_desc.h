#pragma once
#include <stdint.h>
#define ESP_APP_DESC_MAGIC_WORD UINT32_C(0xabcd5432)
typedef struct {
    uint32_t magic_word, secure_version, reserved[2];
    char version[32], project_name[32];
    uint8_t remaining[176];
} esp_app_desc_t;
