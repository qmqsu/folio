#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    FOLIO_ENCODING_UTF8 = 0,
    FOLIO_ENCODING_GBK = 1,
} folio_encoding_t;

// Detect UTF-8 (with or without BOM) or use GBK fallback; seek to first text byte.
// UTF-8 BOM wins even if later bytes are malformed. Reject UTF-16/32 BOM and I/O errors.
bool folio_text_detect(FILE* file, folio_encoding_t* encoding, long* byte_length);

// Reads the next Unicode scalar. Invalid UTF-8/GBK bytes become U+FFFD.
// Returns false at EOF or I/O failure; callers must distinguish errors with ferror(file).
bool folio_text_next(FILE* file, folio_encoding_t encoding, uint32_t* codepoint);

// Decode one UTF-8 sequence from memory. Invalid input consumes one byte.
uint32_t folio_utf8_next(const char** text);

bool folio_is_closing_punctuation(uint32_t codepoint);
bool folio_is_opening_punctuation(uint32_t codepoint);
