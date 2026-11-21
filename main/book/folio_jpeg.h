#pragma once
#include <stddef.h>
#include <stdint.h>

uint8_t *folio_jpeg_fit(const uint8_t *data, size_t size, unsigned max_w, unsigned max_h,
                        unsigned *width, unsigned *height);
