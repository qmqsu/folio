#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOLIO_EPUB_TITLE_CAP 128

typedef struct {
    char title[FOLIO_EPUB_TITLE_CAP];
    uint32_t offset;
} folio_epub_chapter_t;

typedef struct {
    char title[FOLIO_EPUB_TITLE_CAP];
    char text_path[256];
    size_t chapter_count;
    folio_epub_chapter_t* chapters;
} folio_epub_t;

/* Text-only EPUB 2/3 extraction. All offsets point into the generated UTF-8 file.
 * The cache filename includes the source's full CRC32. No archive entry is ever
 * extracted as a filesystem path. result must be freed with folio_epub_free(). */
bool folio_epub_prepare(const char* path, folio_epub_t* result, char* error, size_t error_cap);
void folio_epub_free(folio_epub_t* epub);

/* Extracts a declared PNG/JPEG cover without converting the book text. The
 * caller owns *data and frees it with free(). Both ZIP-compressed and image
 * resource sizes are limited to 2 MiB. No cover: false with an empty error;
 * malformed/unsupported cover: false with a message. Outputs reset on failure.
 * Image decoding and pixel/dimension limits remain the caller's responsibility. */
bool folio_epub_cover(const char* path, uint8_t** data, size_t* size, char* error, size_t error_cap);

/* Reads container + OPF metadata only; does not convert text/create caches.
 * First dc:title / dc:creator, UTF-8-safe truncation. Missing fields are empty.
 * Outputs reset on error. */
bool folio_epub_metadata(const char* path, char* title, size_t title_cap,
                         char* author, size_t author_cap, char* error, size_t error_cap);
