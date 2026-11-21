#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "epdiy.h"

/* Row-major packed gray4: left pixel is the high nibble; 15 is white.
 * width/height describe the entire white, aspect-fit canvas, not just the image.
 * Decode/cover operations must be serialized with the storage/UI lock.
 * A successful decode transfers ownership to the caller; failure leaves *image
 * empty. Initialize the output to zero and free any prior image before reuse. */
typedef struct {
    uint8_t* pixels;
    unsigned width, height;
    size_t stride;
} folio_wallpaper_image_t;

bool folio_wallpaper_decode_file(const char* path, unsigned target_w, unsigned target_h,
                                 folio_wallpaper_image_t* image, char* error, size_t cap);
bool folio_wallpaper_decode_cropped(const char* path, unsigned target_w, unsigned target_h,
                                 folio_wallpaper_image_t* image, char* error, size_t cap);
/* Frees only images returned by decode_file (private tracked allocation).
 * Externally loaded packed canvases may be drawn, but retain their own allocator. */
void folio_wallpaper_image_free(folio_wallpaper_image_t* image);
/* Validate all arguments/cancellation before touching framebuffer; only rect is
 * painted. The image is contained on white if rect differs from its canvas. */
bool folio_wallpaper_draw_image(const folio_wallpaper_image_t* image,
                                uint8_t* framebuffer, EpdRect rect);
