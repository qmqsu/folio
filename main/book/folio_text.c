#include "folio_text.h"

#include <stddef.h>

#include "folio_gbk_table.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
__attribute__((weak,noinline)) bool folio_io_cancelled(void) { return false; }

// Each stream is read by one task only; skip the per-byte stdio lock.
static int read_byte(FILE* file) {
    return getc_unlocked(file);
}

static unsigned read_utf8(FILE* file, uint32_t* out, bool* valid) {
    int b0 = read_byte(file);
    if (b0 == EOF) return 0;
    if (valid != NULL) *valid = true;
    if (b0 < 0x80) {
        *out = (uint32_t)b0;
        return 1;
    }

    unsigned need;
    uint32_t value;
    uint32_t minimum;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1;
        value = (uint32_t)(b0 & 0x1F);
        minimum = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2;
        value = (uint32_t)(b0 & 0x0F);
        minimum = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3;
        value = (uint32_t)(b0 & 0x07);
        minimum = 0x10000;
    } else {
        *out = 0xFFFD;
        if (valid != NULL) *valid = false;
        return 1;
    }

    unsigned consumed = 0;
    while (consumed < need) {
        int bx = read_byte(file);
        if (bx == EOF) {
            if (ferror(file)) return 0;
            *out = 0xFFFD;
            if (valid != NULL) *valid = false;
            return consumed + 1;
        }
        if ((bx & 0xC0) != 0x80) {
            if (ungetc(bx, file) == EOF) return 0;
            *out = 0xFFFD;
            if (valid != NULL) *valid = false;
            return consumed + 1;
        }
        value = (value << 6) | (uint32_t)(bx & 0x3F);
        ++consumed;
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        *out = 0xFFFD;
        if (valid != NULL) *valid = false;
    } else {
        *out = value;
    }
    return consumed + 1;
}

static unsigned read_gbk(FILE* file, uint32_t* out) {
    int lead = read_byte(file);
    if (lead == EOF) return 0;
    if (lead < 0x80) {
        *out = (uint32_t)lead;
        return 1;
    }
    if (lead == 0x80) {
        *out = 0x20AC;
        return 1;
    }
    if (lead < 0x81 || lead > 0xFE) {
        *out = 0xFFFD;
        return 1;
    }

    int trail = read_byte(file);
    if (trail == EOF) {
        if (ferror(file)) return 0;
        *out = 0xFFFD;
        return 1;
    }
    int trail_index;
    if (trail >= 0x40 && trail <= 0x7E) {
        trail_index = trail - 0x40;
    } else if (trail >= 0x80 && trail <= 0xFE) {
        trail_index = 63 + trail - 0x80;
    } else {
        if (ungetc(trail, file) == EOF) return 0;
        *out = 0xFFFD;
        return 1;
    }
    uint16_t mapped = folio_gbk_table[(lead - 0x81) * 190 + trail_index];
    *out = mapped ? mapped : 0xFFFD;
    return 2;
}

bool folio_text_detect(FILE* file, folio_encoding_t* encoding, long* byte_length) {
    if (file == NULL || encoding == NULL || folio_io_cancelled()) return false;
    long original = ftell(file);
    if (original < 0 || fseek(file, 0, SEEK_END) != 0) return false;
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) return false;

    int b0 = read_byte(file);
    int b1 = read_byte(file);
    int b2 = read_byte(file);
    int b3 = read_byte(file);
    if (ferror(file)) return false;
    const bool bom = b0 == 0xEF && b1 == 0xBB && b2 == 0xBF;
    // An explicit UTF-16/32 signature must not be guessed as a GBK book.
    if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF) ||
        (b0 == 0 && b1 == 0 && b2 == 0xFE && b3 == 0xFF)) return false;
    if (fseek(file, bom ? 3 : 0, SEEK_SET) != 0) return false;

    if (bom) {
        *encoding = FOLIO_ENCODING_UTF8;
        if (byte_length != NULL) *byte_length = length;
        return true; // Malformed later bytes are UTF-8 replacement characters.
    }

    bool utf8_valid = true;
    uint32_t cp;
    bool valid = true;
    uint32_t since_yield = 0;
    while (read_utf8(file, &cp, &valid)) {
        if (!valid) {
            utf8_valid = false;
            break;
        }
        if (++since_yield >= 16384) {
            if (folio_io_cancelled()) return false;
#ifdef ESP_PLATFORM
            vTaskDelay(1);
#endif
            since_yield = 0;
        }
    }
    if (ferror(file)) return false;
    *encoding = utf8_valid ? FOLIO_ENCODING_UTF8 : FOLIO_ENCODING_GBK;
    if (byte_length != NULL) *byte_length = length;
    return fseek(file, bom ? 3 : 0, SEEK_SET) == 0 && original >= 0;
}

unsigned folio_text_next_bytes(FILE* file, folio_encoding_t encoding, uint32_t* codepoint) {
    if (file == NULL || codepoint == NULL) return false;
    if (encoding == FOLIO_ENCODING_GBK) return read_gbk(file, codepoint);
    return read_utf8(file, codepoint, NULL);
}

bool folio_text_next(FILE* file, folio_encoding_t encoding, uint32_t* codepoint) {
    return folio_text_next_bytes(file, encoding, codepoint) != 0;
}

uint32_t folio_utf8_next(const char** text) {
    if (text == NULL || *text == NULL || **text == '\0') return 0;
    const unsigned char* p = (const unsigned char*)*text;
    uint32_t value;
    unsigned need;
    uint32_t minimum;
    if (p[0] < 0x80) {
        *text += 1;
        return p[0];
    } else if (p[0] >= 0xC2 && p[0] <= 0xDF) {
        value = p[0] & 0x1F; need = 1; minimum = 0x80;
    } else if (p[0] >= 0xE0 && p[0] <= 0xEF) {
        value = p[0] & 0x0F; need = 2; minimum = 0x800;
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        value = p[0] & 0x07; need = 3; minimum = 0x10000;
    } else {
        *text += 1;
        return 0xFFFD;
    }
    for (unsigned i = 1; i <= need; ++i) {
        if (p[i] == 0 || (p[i] & 0xC0) != 0x80) {
            *text += 1;
            return 0xFFFD;
        }
        value = (value << 6) | (p[i] & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        *text += 1;
        return 0xFFFD;
    }
    *text += need + 1;
    return value;
}

bool folio_is_closing_punctuation(uint32_t cp) {
    return cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C || cp == 0xFF0E || cp == 0xFF01 ||
           cp == 0xFF1F || cp == 0xFF1A || cp == 0xFF1B || cp == 0xFF09 || cp == 0x3011 ||
           cp == 0x300D || cp == 0x300F || cp == 0x3015 || cp == 0x3017 || cp == 0x3019 ||
           cp == 0x301B || cp == 0x2019 || cp == 0x201D;
}

bool folio_is_opening_punctuation(uint32_t cp) {
    return cp == 0xFF08 || cp == 0x3010 || cp == 0x300C || cp == 0x300E || cp == 0x3014 ||
           cp == 0x3016 || cp == 0x3018 || cp == 0x301A || cp == 0x2018 || cp == 0x201C;
}
