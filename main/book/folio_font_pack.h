#ifndef FOLIO_FONT_PACK_H
#define FOLIO_FONT_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *compressed;
    size_t compressed_size;
    size_t raw_size;
    uint32_t compressed_crc32;
    uint32_t raw_crc32;
} folio_font_resource_t;

/* IEEE CRC-32, compatible with zlib crc32(0, bytes, length).
 * Empty input and NULL return zero; non-NULL input must span length bytes.
 * No allocation, mutable globals, or initialization is required.
 */
uint32_t folio_font_pack_crc32(const void *bytes, size_t length);

/* Find hei, kai, yuan, or symbols in a complete 5242880-byte FOLFONT1 image.
 * Validates the v1 header, all entry bounds/reserved bytes, metadata CRC/UTF-8,
 * metadata padding, final 64 KiB erased guard, and selected compressed CRC.
 * Metadata JSON/schema, SHA-256, and payload-gap padding are host concerns.
 * CRC detects corruption; it is not authentication. The caller must inflate
 * raw DEFLATE, enforce raw_size, and verify raw_crc32 before using the font.
 * The borrowed compressed pointer stays valid only while bytes stays mapped
 * and immutable. id must be a NUL-terminated string. out must not alias input.
 * On failure returns false and clears *out (unless out itself is NULL).
 * Pure C, allocation-free, bounded stack, and safe with unaligned image bytes.
 */
bool folio_font_pack_find(const void *bytes, size_t length, const char *id,
                          folio_font_resource_t *out);

#ifdef __cplusplus
}
#endif

#endif
