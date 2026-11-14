#pragma once
#include <stdint.h>
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_IMAGE_MAX_SEGMENTS 16
#define ESP_CHIP_ID_ESP32S3 9
typedef struct { uint8_t bytes[24]; } esp_image_header_t;
typedef struct { uint32_t address, length; } esp_image_segment_header_t;
