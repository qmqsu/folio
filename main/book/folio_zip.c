#include "folio_zip.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef FOLIO_EPUB_HOST_TEST
#include <zlib.h>
#else
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"
#endif

#define ZIP_MAX_ENTRIES 2048u
#define ZIP_MAX_NAMES (128u * 1024u)
#define ZIP_MAX_CENTRAL (512u * 1024u)
#define ZIP_MAX_ARCHIVE (128u * 1024u * 1024u)
#define ZIP_MAX_MEMBER (8u * 1024u * 1024u)
#define ZIP_INPUT_BYTES 4096u
__attribute__((weak, noinline)) bool folio_io_cancelled(void) { return false; }
bool folio_epub_cancelled(void) { return folio_io_cancelled(); }

void* folio_epub_alloc(size_t size) {
#ifndef FOLIO_EPUB_HOST_TEST
    void* memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory != NULL) return memory;
#endif
    return malloc(size);
}

void folio_epub_yield(void) {
#ifndef FOLIO_EPUB_HOST_TEST
    vTaskDelay(1);
#endif
}

uint32_t folio_epub_crc32(uint32_t previous, const void* bytes, size_t count) {
    uint32_t crc = ~previous;
    const uint8_t* p = bytes;
    static uint32_t table[256];
    static bool initialized;
    if (!initialized) {
        for (unsigned n = 0; n < 256; ++n) {
            uint32_t value = n;
            for (unsigned b = 0; b < 8; ++b) value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1)));
            table[n] = value;
        }
        initialized = true;
    }
    for (size_t i = 0; i < count; ++i) crc = table[(crc ^ p[i]) & 255u] ^ (crc >> 8);
    return ~crc;
}

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool fail(char* error, size_t cap, const char* message) {
    if (error != NULL && cap != 0) snprintf(error, cap, "%s", message);
    return false;
}
static bool read_at(FILE* f, uint32_t offset, void* target, size_t count) {
    if (offset > ZIP_MAX_ARCHIVE || fseek(f, (long)offset, SEEK_SET)) return false;
    uint8_t* output = target;
    while (count) {
        if (folio_epub_cancelled()) return false;
        size_t n = count > ZIP_INPUT_BYTES ? ZIP_INPUT_BYTES : count;
        if (fread(output, 1, n, f) != n) return false;
        output += n; count -= n;
    }
    return !folio_epub_cancelled();
}
static bool safe_name(const uint8_t* name, size_t length) {
    if (length == 0 || length >= FOLIO_ZIP_NAME_CAP || name[0] == '/') return false;
    size_t segment = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && (name[i] < 32 || name[i] == 127 || name[i] == '\\' || name[i] == ':')) return false;
        if (i == length || name[i] == '/') {
            size_t n = i - segment;
            if ((n == 0 && i != length) || (n == 1 && name[segment] == '.') ||
                (n == 2 && name[segment] == '.' && name[segment + 1] == '.')) return false;
            segment = i + 1;
        }
    }
    return true;
}
static bool extra_valid(const uint8_t* bytes, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        if (size - pos < 4) return false;
        uint16_t type = le16(bytes + pos), count = le16(bytes + pos + 2);
        pos += 4;
        if (type == 0x0001 || count > size - pos) return false; // ZIP64 excluded.
        pos += count;
    }
    return true;
}

void folio_zip_close(folio_zip_t* zip) {
    if (zip == NULL) return;
    if (zip->file != NULL) fclose(zip->file);
    free(zip->entries);
    free(zip->names);
    memset(zip, 0, sizeof(*zip));
}

static bool open_zip(const char* path, folio_zip_t* zip, char* error, size_t cap, bool fingerprint) {
    if (zip == NULL || path == NULL) return fail(error, cap, "Invalid EPUB path");
    memset(zip, 0, sizeof(*zip));
    zip->file = fopen(path, "rb");
    if (zip->file == NULL) return fail(error, cap, "Cannot open EPUB");
    uint8_t* tail = NULL;
    uint8_t* central = NULL;
    const char* reason = "Invalid ZIP directory";
    if (fseek(zip->file, 0, SEEK_END) != 0) goto failed;
    long file_length = ftell(zip->file);
    if (file_length < 22 || (unsigned long)file_length > ZIP_MAX_ARCHIVE) {
        reason = "EPUB exceeds 128 MiB or is truncated";
        goto failed;
    }
    zip->source_size = (uint32_t)file_length;
    size_t tail_size = zip->source_size < 65557u ? zip->source_size : 65557u;
    tail = folio_epub_alloc(tail_size);
    if (tail == NULL) { reason = "Not enough memory for ZIP"; goto failed; }
    if (!read_at(zip->file, zip->source_size - (uint32_t)tail_size, tail, tail_size)) goto failed;
    size_t eocd = SIZE_MAX;
    for (size_t pos = tail_size - 22;; --pos) {
        if (le32(tail + pos) == 0x06054B50u && pos + 22u + le16(tail + pos + 20) == tail_size) {
            eocd = pos;
            break;
        }
        if (pos == 0) break;
    }
    if (eocd == SIZE_MAX) goto failed;
    const uint8_t* end = tail + eocd;
    uint32_t central_size = le32(end + 12), central_offset = le32(end + 16);
    uint32_t eocd_offset = zip->source_size - (uint32_t)tail_size + (uint32_t)eocd;
    uint16_t count = le16(end + 10);
    if (le16(end + 4) != 0 || le16(end + 6) != 0 || le16(end + 8) != count ||
        count == 0 || count > ZIP_MAX_ENTRIES || central_size > ZIP_MAX_CENTRAL ||
        central_offset > eocd_offset || central_size != eocd_offset - central_offset) goto failed;
    if (eocd >= 20 && le32(tail + eocd - 20) == 0x07064B50u) goto failed;
    free(tail); tail = NULL;
    central = folio_epub_alloc(central_size);
    if (central == NULL) { reason = "Not enough memory for ZIP"; goto failed; }
    if (!read_at(zip->file, central_offset, central, central_size)) goto failed;
    size_t scan = 0, name_capacity = 0;
    for (size_t i = 0; i < count; ++i) {
        if (folio_epub_cancelled()) goto failed;
        if (central_size - scan < 46 || le32(central + scan) != 0x02014B50u) goto failed;
        uint16_t name_length = le16(central + scan + 28);
        uint32_t record_size = 46u + name_length + le16(central + scan + 30) + le16(central + scan + 32);
        if (record_size > central_size - scan || (size_t)name_length + 1 > ZIP_MAX_NAMES - name_capacity) goto failed;
        name_capacity += name_length + 1;
        scan += record_size;
    }
    if (scan != central_size) goto failed;
    zip->entries = folio_epub_alloc((size_t)count * sizeof(*zip->entries));
    zip->names = folio_epub_alloc(name_capacity);
    if (zip->entries == NULL || zip->names == NULL) {
        reason = "Not enough memory for ZIP"; goto failed;
    }
    memset(zip->entries, 0, (size_t)count * sizeof(*zip->entries));
    size_t pos = 0, name_used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (folio_epub_cancelled()) goto failed;
        if (central_size - pos < 46 || le32(central + pos) != 0x02014B50u) goto failed;
        const uint8_t* c = central + pos;
        uint16_t name_length = le16(c + 28), extra_length = le16(c + 30), comment_length = le16(c + 32);
        uint32_t record_size = 46u + name_length + extra_length + comment_length;
        if (record_size > central_size - pos || le16(c + 34) != 0 ||
            !safe_name(c + 46, name_length) || !extra_valid(c + 46 + name_length, extra_length) ||
            (size_t)name_length + 1 > name_capacity - name_used) goto failed;
        folio_zip_entry_t* entry = &zip->entries[i];
        entry->flags = le16(c + 8); entry->method = le16(c + 10);
        entry->crc = le32(c + 16); entry->compressed = le32(c + 20);
        entry->size = le32(c + 24); entry->local_offset = le32(c + 42);
        if ((entry->flags & ~0x081Eu) != 0 || (entry->method != 0 && entry->method != 8) ||
            (entry->method == 0 && entry->compressed != entry->size) ||
            entry->compressed > ZIP_MAX_MEMBER || entry->size > ZIP_MAX_MEMBER ||
            entry->local_offset > central_offset || central_offset - entry->local_offset < 30 ||
            ((le32(c + 38) >> 16) & 0xF000u) == 0xA000u) {
            reason = "Unsupported, encrypted or oversized ZIP member"; goto failed;
        }
        entry->name = zip->names + name_used;
        memcpy(entry->name, c + 46, name_length); entry->name[name_length] = '\0';
        name_used += name_length + 1;
        uint8_t local[30], local_name[FOLIO_ZIP_NAME_CAP];
        if (!read_at(zip->file, entry->local_offset, local, sizeof(local)) ||
            le32(local) != 0x04034B50u || le16(local + 6) != entry->flags ||
            le16(local + 8) != entry->method || le16(local + 26) != name_length) goto failed;
        uint16_t local_extra_length = le16(local + 28);
        uint32_t header_size = 30u + name_length + local_extra_length;
        if (header_size > central_offset - entry->local_offset) goto failed;
        entry->data_offset = entry->local_offset + header_size;
        if (entry->compressed > central_offset - entry->data_offset ||
            !read_at(zip->file, entry->local_offset + 30, local_name, name_length) ||
            memcmp(local_name, entry->name, name_length) != 0) goto failed;
        if ((entry->flags & 8) == 0 && (le32(local + 14) != entry->crc ||
            le32(local + 18) != entry->compressed || le32(local + 22) != entry->size)) goto failed;
        if ((entry->flags & 8) != 0 &&
            ((le32(local + 14) != 0 && le32(local + 14) != entry->crc) ||
             (le32(local + 18) != 0 && le32(local + 18) != entry->compressed) ||
             (le32(local + 22) != 0 && le32(local + 22) != entry->size))) goto failed;
        entry->member_end = entry->data_offset + entry->compressed;
        if ((entry->flags & 8) != 0) {
            uint8_t descriptor[16];
            uint32_t available = central_offset - entry->member_end;
            if (available < 12 || !read_at(zip->file, entry->member_end, descriptor, 12)) goto failed;
            bool unsigned_descriptor = le32(descriptor) == entry->crc &&
                le32(descriptor + 4) == entry->compressed && le32(descriptor + 8) == entry->size;
            if (unsigned_descriptor) entry->member_end += 12;
            else {
                if (le32(descriptor) != 0x08074B50u || available < 16 ||
                    !read_at(zip->file, entry->member_end, descriptor, 16) ||
                    le32(descriptor + 4) != entry->crc || le32(descriptor + 8) != entry->compressed ||
                    le32(descriptor + 12) != entry->size) goto failed;
                entry->member_end += 16;
            }
        }
        if (local_extra_length != 0) {
            uint8_t* extra = folio_epub_alloc(local_extra_length);
            if (extra == NULL) { reason = "Not enough memory for ZIP"; goto failed; }
            bool okay = read_at(zip->file, entry->local_offset + 30u + name_length, extra, local_extra_length) &&
                        extra_valid(extra, local_extra_length);
            free(extra);
            if (!okay) goto failed;
        }
        for (size_t prior = 0; prior < i; ++prior) {
            const folio_zip_entry_t* other = &zip->entries[prior];
            if (strcmp(other->name, entry->name) == 0 ||
                (entry->local_offset < other->member_end && other->local_offset < entry->member_end)) goto failed;
        }
        pos += record_size;
        zip->count++;
        if ((i & 31) == 0) folio_epub_yield();
    }
    if (pos != central_size) goto failed;
    free(central); central = NULL;
    if (!fingerprint) return true;
    uint8_t* buffer = folio_epub_alloc(ZIP_INPUT_BYTES);
    if (buffer == NULL) { reason = "Not enough memory for EPUB fingerprint"; goto failed; }
    if (fseek(zip->file, 0, SEEK_SET) != 0) { free(buffer); goto failed; }
    uint32_t remaining = zip->source_size;
    while (remaining > 0) {
        if (folio_epub_cancelled()) { free(buffer); goto failed; }
        size_t n = remaining > ZIP_INPUT_BYTES ? ZIP_INPUT_BYTES : remaining;
        if (fread(buffer, 1, n, zip->file) != n) { free(buffer); goto failed; }
        zip->source_crc = folio_epub_crc32(zip->source_crc, buffer, n);
        remaining -= (uint32_t)n;
        if ((remaining & 0xFFFFu) < ZIP_INPUT_BYTES) folio_epub_yield();
    }
    free(buffer);
    return true;
failed:
    free(tail); free(central); folio_zip_close(zip);
    if (folio_epub_cancelled()) reason = "EPUB operation cancelled";
    return fail(error, cap, reason);
}

bool folio_zip_open(const char* path, folio_zip_t* zip, char* error, size_t cap) {
    return open_zip(path, zip, error, cap, true);
}

bool folio_zip_open_directory(const char* path, folio_zip_t* zip, char* error, size_t cap) {
    return open_zip(path, zip, error, cap, false);
}

const folio_zip_entry_t* folio_zip_find(const folio_zip_t* zip, const char* name) {
    if (zip == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < zip->count; ++i) {
        if (strcmp(zip->entries[i].name, name) == 0) return &zip->entries[i];
    }
    return NULL;
}

static bool inflate_entry(FILE* file, const folio_zip_entry_t* entry, uint8_t* out) {
    uint8_t* input = folio_epub_alloc(ZIP_INPUT_BYTES);
    if (input == NULL) return false;
    uint32_t remaining = entry->compressed;
    size_t input_pos = 0, input_count = 0, output_pos = 0;
    bool okay = false;
#ifdef FOLIO_EPUB_HOST_TEST
    z_stream stream = {0};
    if (inflateInit2(&stream, -15) != Z_OK) { free(input); return false; }
#else
    tinfl_decompressor* stream = folio_epub_alloc(sizeof(*stream));
    if (stream == NULL) { free(input); return false; }
    memset(stream, 0, sizeof(*stream)); tinfl_init(stream);
#endif
    for (;;) {
        if (folio_epub_cancelled()) break;
        if (input_pos == input_count && remaining > 0) {
            input_count = remaining > ZIP_INPUT_BYTES ? ZIP_INPUT_BYTES : remaining;
            if (fread(input, 1, input_count, file) != input_count) break;
            remaining -= (uint32_t)input_count; input_pos = 0;
        }
        size_t in_size = input_count - input_pos;
        size_t out_size = (size_t)entry->size + 1 - output_pos;
        if (out_size > 64u * 1024u) out_size = 64u * 1024u;
        size_t consumed, produced;
        bool done, failed;
#ifdef FOLIO_EPUB_HOST_TEST
        stream.next_in = input + input_pos; stream.avail_in = (uInt)in_size;
        stream.next_out = out + output_pos; stream.avail_out = (uInt)out_size;
        int status = inflate(&stream, Z_NO_FLUSH);
        consumed = in_size - stream.avail_in; produced = out_size - stream.avail_out;
        done = status == Z_STREAM_END; failed = status < 0;
#else
        uint32_t flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF |
                         (remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        int status = tinfl_decompress(stream, input + input_pos, &in_size, out,
                                      out + output_pos, &out_size, flags);
        consumed = in_size; produced = out_size;
        done = status == TINFL_STATUS_DONE; failed = status < TINFL_STATUS_DONE;
#endif
        input_pos += consumed; output_pos += produced;
        if (done) {
            okay = output_pos == entry->size && input_pos == input_count && remaining == 0;
            break;
        }
        if (failed || output_pos > entry->size || (consumed == 0 && produced == 0)) break;
        folio_epub_yield();
    }
#ifdef FOLIO_EPUB_HOST_TEST
    inflateEnd(&stream);
#else
    free(stream);
#endif
    free(input);
    return okay;
}

uint8_t* folio_zip_read(const folio_zip_t* zip, const folio_zip_entry_t* entry,
                        size_t limit, size_t* size, char* error, size_t cap) {
    if (folio_epub_cancelled()) { fail(error, cap, "EPUB operation cancelled"); return NULL; }
    if (zip == NULL || entry == NULL) { fail(error, cap, "EPUB resource is missing"); return NULL; }
    if (entry->size > limit || entry->compressed > ZIP_MAX_MEMBER) {
        fail(error, cap, "EPUB chapter or metadata exceeds 512 KiB"); return NULL;
    }
    uint8_t* output = folio_epub_alloc((size_t)entry->size + 1);
    if (output == NULL) { fail(error, cap, "Not enough memory for EPUB chapter"); return NULL; }
    bool okay = fseek(zip->file, (long)entry->data_offset, SEEK_SET) == 0;
    if (okay) {
        okay = entry->method == 0 ? read_at(zip->file, entry->data_offset, output, entry->size) :
                                  inflate_entry(zip->file, entry, output);
    }
    uint32_t crc = 0;
    for (size_t offset = 0; okay && offset < entry->size; offset += ZIP_INPUT_BYTES) {
        if (folio_epub_cancelled()) { okay = false; break; }
        size_t count = entry->size - offset;
        if (count > ZIP_INPUT_BYTES) count = ZIP_INPUT_BYTES;
        crc = folio_epub_crc32(crc, output + offset, count);
    }
    if (!okay || crc != entry->crc) {
        free(output); fail(error, cap, folio_epub_cancelled() ? "EPUB operation cancelled" : "EPUB decompression or CRC check failed"); return NULL;
    }
    output[entry->size] = '\0';
    if (size != NULL) *size = entry->size;
    return output;
}

bool folio_epub_valid_mime(const uint8_t* data, size_t size) {
    return data && (size == 20 || (size == 21 && data[20] == '\n') ||
        (size == 22 && data[20] == '\r' && data[21] == '\n')) &&
        !memcmp(data,"application/epub+zip",20);
}
