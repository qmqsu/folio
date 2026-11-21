#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define FOLIO_ZIP_NAME_CAP 256
#define FOLIO_EPUB_XML_LIMIT (512u * 1024u)

typedef struct {
    char* name;
    uint32_t crc;
    uint32_t compressed;
    uint32_t size;
    uint32_t local_offset;
    uint32_t data_offset;
    uint32_t member_end;
    uint16_t flags;
    uint16_t method;
} folio_zip_entry_t;

typedef struct {
    FILE* file;
    folio_zip_entry_t* entries;
    char* names;
    size_t count;
    uint64_t source_hash;
    uint32_t source_crc; // Full archive CRC from folio_zip_open; zero for directory-only open.
    uint32_t source_size;
} folio_zip_t;

void* folio_epub_alloc(size_t size);
void folio_epub_yield(void);
/* Optional application bool folio_io_cancelled(void) weak hook. */
bool folio_epub_cancelled(void);
uint32_t folio_epub_crc32(uint32_t previous, const void* bytes, size_t count);
bool folio_zip_open(const char* path, folio_zip_t* zip, char* error, size_t cap);
/* Same complete central/local/header/path/overlap validation as folio_zip_open,
 * without scanning all archive bytes for a fingerprint. source_crc stays zero
 * and must not be used as cache/progress identity. folio_zip_read still verifies
 * each extracted member's CRC. Intended for fast cover/metadata discovery. */
bool folio_zip_open_directory(const char* path, folio_zip_t* zip, char* error, size_t cap);
void folio_zip_close(folio_zip_t* zip);
const folio_zip_entry_t* folio_zip_find(const folio_zip_t* zip, const char* name);
uint8_t* folio_zip_read(const folio_zip_t* zip, const folio_zip_entry_t* entry,
                        size_t limit, size_t* size, char* error, size_t cap);

/* Permit the optional line ending used by older EPUB builders. */
bool folio_epub_valid_mime(const uint8_t* data, size_t size);
