#pragma once
#include <stdlib.h>
#include <stdbool.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
// Tests may simulate a full PSRAM heap while ordinary malloc remains available.
#ifndef folio_test_psram_exhausted
__attribute__((weak)) bool folio_test_psram_exhausted(void) { return false; }
#endif
static inline void* heap_caps_malloc(size_t size, unsigned caps) {
    if ((caps & MALLOC_CAP_SPIRAM) && folio_test_psram_exhausted()) return NULL;
    return malloc(size);
}
