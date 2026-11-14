#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "epdiy.h"

/* Draw an EPUB embedded cover, or a same-basename .jpg/.jpeg/.png sidecar.
 * Images retain their aspect ratio and are composited against white.
 * Returns false when absent, unsupported, corrupt, too large or out of memory.
 * Single UI task only. Positive/negative results are cached; source stat changes
 * invalidate a cache entry. Call clear after card reload/rescan and at shutdown. */
bool folio_cover_draw(const char* book_path, uint8_t* framebuffer, EpdRect rect);
void folio_cover_clear_cache(void);
// Fully decodes a bounded PNG/JPEG file without changing the cover cache.
// Serialize with cover drawing (the transfer task holds the storage lock).
bool folio_cover_validate_file(const char* path);
// Draw a validated direct PNG/JPEG at display resolution, aspect-fit on white.
// UI task only; caller supplies a storage-validated path. No cover cache entry.
bool folio_cover_draw_image(const char* path, uint8_t* framebuffer, EpdRect rect);
/* Compatibility only: original <=2 MiB/2 MP JPEG path, no expanded limits.
 * Returns an owned gray8 aspect-fit image; caller frees it with free().
 * This whole-file/stb path is NOT covered by the streaming wallpaper budget.
 * peak_bytes includes input, live decoder allocations and the result image.
 * cancelled, when supplied, is checked by the existing I/O/entropy/row hooks
 * and is reset before returning; ordinary covers never use this callback.
 * Serialize with all other cover and wallpaper operations. */
uint8_t* folio_cover_decode_legacy_jpeg_file(const char* path, unsigned max_w, unsigned max_h,
                                           unsigned* width, unsigned* height, size_t* peak_bytes,
                                           bool (*cancelled)(void*), void* cancel_context);
