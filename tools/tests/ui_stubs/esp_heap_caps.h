#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void* heap_caps_malloc(size_t bytes, unsigned flags) { (void)flags; return malloc(bytes); }
static inline void* heap_caps_calloc(size_t count, size_t bytes, unsigned flags) { (void)flags; return calloc(count, bytes); }

#define MALLOC_CAP_INTERNAL 4
static inline size_t heap_caps_get_free_size(unsigned flags){(void)flags;return 65536;}
static inline size_t heap_caps_get_largest_free_block(unsigned flags){(void)flags;return 32768;}
// Fixed diagnostics only, not a simulated heap budget or a hardware measurement.
static inline size_t heap_caps_get_minimum_free_size(unsigned flags){(void)flags;return 32768;}
