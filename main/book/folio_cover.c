#include "folio_cover.h"
#include "folio_weread_paths.h"
#include "folio_epub.h"
#include "esp_heap_caps.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define COVER_INPUT_MAX (2u * 1024u * 1024u)
#define COVER_WORK_MAX (4u * 1024u * 1024u)
#define COVER_PIXEL_MAX 2000000u
#define COVER_DIM_MAX 4096u
#define COVER_W 192u
#define COVER_H 288u
#define COVER_BOOKS 24
#define COVER_IMAGES 8
#define COVER_PATH 256

static size_t s_work_live, s_work_peak;
/* Set only by the wallpaper compatibility bridge; ordinary covers are unchanged. */
static bool s_legacy_psram_only;
static bool (*s_legacy_cancel)(void*);
static void* s_legacy_cancel_context;
__attribute__((weak,noinline)) bool folio_io_cancelled(void) { return false; }
static bool cover_cancelled(void) {
    return folio_io_cancelled() || (s_legacy_cancel && s_legacy_cancel(s_legacy_cancel_context));
}
static void cover_yield(void) {
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
}
static bool cover_jpeg_cancelled(void) {
    static unsigned rows;
    if(cover_cancelled())return true;
    if(!(++rows & 15u))cover_yield();
    return false;
}
static void* cover_alloc(size_t size) {
#ifdef ESP_PLATFORM
    if (s_legacy_psram_only) {
        size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t reserve = 384u * 1024u;
        if (available < reserve || size > available - reserve) return NULL;
    }
#endif
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL && !s_legacy_psram_only) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return p;
}
typedef union { size_t size; max_align_t alignment; } cover_alloc_header_t;
static void* cover_work_alloc(size_t size) {
    if (size == 0 || size > COVER_WORK_MAX - s_work_live || size > SIZE_MAX - sizeof(cover_alloc_header_t)) return NULL;
    cover_alloc_header_t* p = cover_alloc(size + sizeof(*p));
    if (p == NULL) return NULL;
    p->size = size; s_work_live += size;
    if (s_work_live > s_work_peak) s_work_peak = s_work_live;
    // Corrupt/partial JPEG scans must never expose uninitialized source planes.
    memset(p + 1, 0, size);
    return p + 1;
}
static void cover_work_free(void* p) {
    if (p == NULL) return;
    cover_alloc_header_t* header = (cover_alloc_header_t*)p - 1;
    s_work_live -= header->size; free(header);
}
static void* cover_work_calloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    size *= count;
    void* p = cover_work_alloc(size);
    if (p) memset(p, 0, size);
    return p;
}
static __attribute__((unused)) void* cover_work_realloc(void* old, size_t size) {
    if (!old) return cover_work_alloc(size);
    if (!size) { cover_work_free(old); return NULL; }
    cover_alloc_header_t* header = (cover_alloc_header_t*)old - 1;
    void* p = cover_work_alloc(size);
    if (!p) return NULL;
    memcpy(p, old, size < header->size ? size : header->size);
    cover_work_free(old); return p;
}
static size_t s_png_remaining;
static bool s_png_finished;
static bool cover_png_output_check(size_t bytes) {
    if (bytes > s_png_remaining) return false;
    s_png_remaining -= bytes; return true;
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS COVER_DIM_MAX
#define STBI_MALLOC cover_work_alloc
#define STBI_REALLOC cover_work_realloc
#define STBI_FREE cover_work_free
#define STBI_JPEG_CANCELLED() cover_jpeg_cancelled()
#include "vendor/stb_image.h"

// Keep this private inflater separate from the ESP ROM miniz ZIP decoder.
#define mz_crc32 folio_cover_mz_crc32
#define mz_adler32 folio_cover_mz_adler32
#define mz_free folio_cover_mz_free
#define tinfl_decompress folio_cover_tinfl_decompress
#define tinfl_decompress_mem_to_heap folio_cover_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem folio_cover_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback folio_cover_tinfl_decompress_mem_to_callback
#define MINIZ_NO_MALLOC
#include "vendor/pngle/miniz.c"
#define calloc cover_work_calloc
#define free cover_work_free
#define PNGLE_NO_GAMMA_CORRECTION
#define PNGLE_OUTPUT_CHECK cover_png_output_check
#define PNGLE_INFLATE_STATUS(status) do { if ((status) == TINFL_STATUS_DONE) s_png_finished = true; } while (0)
#include "vendor/pngle/pngle.c"
#undef calloc
#undef free
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct { uint32_t sum, count; } cover_sample_t;
typedef struct {
    unsigned width, height, source_w, source_h;
    cover_sample_t* samples;
    uint8_t* nearest;
    uint64_t pixels;
    bool done, invalid;
} cover_reduce_t;
typedef struct { uint8_t* pixels; unsigned width, height; uint64_t age; } cover_image_t;
typedef struct {
    char path[COVER_PATH];
    uint64_t stamp, age;
    cover_image_t* image;
    bool used;
} cover_book_t;
static cover_book_t s_books[COVER_BOOKS];
static cover_image_t s_images[COVER_IMAGES];
static uint64_t s_age;

static uint32_t cover_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static bool cover_dimensions(unsigned w, unsigned h) {
    return w && h && w <= COVER_DIM_MAX && h <= COVER_DIM_MAX && (uint64_t)w * h <= COVER_PIXEL_MAX;
}
static void cover_target(unsigned sw, unsigned sh, unsigned* w, unsigned* h, unsigned max_w, unsigned max_h) {
    *w = sw; *h = sh;
    if (*w > max_w) { *h = (unsigned)((uint64_t)sh * max_w / sw); *w = max_w; }
    if (*h > max_h) { *w = (unsigned)((uint64_t)sw * max_h / sh); *h = max_h; }
    if (!*w) *w = 1;
    if (!*h) *h = 1;
}
static void cover_reduce_pixel(cover_reduce_t* r, unsigned x, unsigned y, unsigned gray) {
    if (x >= r->source_w || y >= r->source_h) { r->invalid = true; return; }
    // Prevalidated source <=4096 and thumbnail <=288 keep products in uint32.
    unsigned dx = x * r->width / r->source_w;
    unsigned dy = y * r->height / r->source_h;
    if(r->nearest)r->nearest[(size_t)dy*r->width+dx]=(uint8_t)gray;
    else {
        cover_sample_t* sample = &r->samples[(size_t)dy * r->width + dx];
        sample->sum += gray; ++sample->count;
    }
    ++r->pixels;
}
static void cover_png_pixel(pngle_t* png, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    cover_reduce_t* r = pngle_get_user_data(png);
    if (w != 1 || h != 1) { r->invalid = true; return; }
    unsigned gray = (77u * rgba[0] + 150u * rgba[1] + 29u * rgba[2] + 128u) >> 8;
    gray = (gray * rgba[3] + 255u * (255u - rgba[3]) + 127u) / 255u;
    cover_reduce_pixel(r, x, y, gray);
    if (x == 0 && !(y & 31)) cover_yield();
}
static void cover_png_done(pngle_t* png) { ((cover_reduce_t*)pngle_get_user_data(png))->done = true; }
static bool cover_png_header(const uint8_t* data, size_t size, unsigned* w, unsigned* h, unsigned* channels) {
    static const uint8_t signature[] = {137,80,78,71,13,10,26,10};
    if (size < 33 || memcmp(data, signature, 8) || cover_be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4)) return false;
    *w = cover_be32(data + 16); *h = cover_be32(data + 20);
    if (!cover_dimensions(*w, *h) || data[24] != 8 || data[26] || data[27] || data[28]) return false;
    switch (data[25]) {
        case 0: case 3: *channels = 1; break;
        case 2: *channels = 3; break;
        case 4: *channels = 2; break;
        case 6: *channels = 4; break;
        default: return false;
    }
    // Reject unsupported animation, invalid chunk bounds and unknown critical chunks.
    size_t pos = 8; bool ended = false;
    while (pos < size) {
        if (size - pos < 12) return false;
        uint32_t length = cover_be32(data + pos);
        if (length > size - pos - 12) return false;
        const uint8_t* type = data + pos + 4;
        if (!memcmp(type, "acTL", 4)) return false;
        if (!(type[0] & 32) && memcmp(type,"IHDR",4) && memcmp(type,"PLTE",4) && memcmp(type,"IDAT",4) && memcmp(type,"IEND",4)) return false;
        if (!memcmp(type, "IEND", 4)) { ended = length == 0 && pos + 12 == size; break; }
        pos += (size_t)length + 12;
    }
    return ended;
}
static bool cover_jpeg_structure(const uint8_t* data, size_t size) {
    if (size < 4 || data[0] != 255 || data[1] != 216) return false;
    size_t pos = 2, scan_bytes = 0;
    bool frame = false, scan = false, entropy = false, progressive = false;
    unsigned components = 0, component_ids[4] = {0}, scan_mask = 0;
    while (pos < size) {
        if (entropy) while (pos < size && data[pos] != 255) { ++pos; ++scan_bytes; }
        if (pos == size || data[pos++] != 255) return false;
        while (pos < size && data[pos] == 255) ++pos;
        if (pos == size) return false;
        unsigned marker = data[pos++];
        if (entropy && marker == 0) { ++scan_bytes; continue; }
        if (entropy && marker >= 0xD0 && marker <= 0xD7) continue;
        if (entropy && !scan_bytes) return false;
        entropy = false;
        if (marker == 0xD9) return frame && scan && pos == size && scan_mask == (1u << components) - 1;
        if (marker == 0 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || size - pos < 2) return false;
        if (marker == 1) continue;
        size_t length = (size_t)data[pos] * 256 + data[pos + 1];
        if (length < 2 || length > size - pos) return false;
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (frame || length < 11 || data[pos + 2] != 8 || !data[pos + 7] || data[pos + 7] > 4 || length != 8u + 3u * data[pos + 7]) return false;
            if (marker != 0xC0 && marker != 0xC1 && marker != 0xC2) return false;
            frame = true; progressive = marker == 0xC2; components = data[pos + 7];
            for (unsigned i = 0; i < components; ++i) {
                component_ids[i] = data[pos + 8 + i * 3];
                for (unsigned j = 0; j < i; ++j) if (component_ids[i] == component_ids[j]) return false;
            }
        } else if (marker == 0xDA) {
            if (!frame || length < 8 || !data[pos + 2] || length != 6u + 2u * data[pos + 2]) return false;
            unsigned this_scan = 0;
            for (unsigned i = 0; i < data[pos + 2]; ++i) {
                unsigned id = data[pos + 3 + i * 2], j = 0;
                while (j < components && component_ids[j] != id) ++j;
                if (j == components || (this_scan & (1u << j))) return false;
                this_scan |= 1u << j;
            }
            if (!progressive || (data[pos + length - 3] == 0 && (data[pos + length - 1] >> 4) == 0)) scan_mask |= this_scan;
            scan = true; entropy = true; scan_bytes = 0;
        }
        pos += length;
    }
    return false;
}
static uint8_t* cover_decode_target(const uint8_t* bytes, size_t size, unsigned* width, unsigned* height,
                                   unsigned max_w,unsigned max_h,bool nearest) {
    if (!bytes || !size || size > COVER_INPUT_MAX || cover_cancelled()) return NULL;
    unsigned sw = 0, sh = 0, channels = 0;
    bool png = bytes[0] == 137;
    uint8_t* jpeg = NULL;
    if (png) {
        if (!cover_png_header(bytes, size, &sw, &sh, &channels)) return NULL;
    } else {
        if (!cover_jpeg_structure(bytes, size)) return NULL;
        int w = 0, h = 0, n = 0;
        if (!stbi_info_from_memory(bytes, (int)size, &w, &h, &n) || w <= 0 || h <= 0 || !cover_dimensions((unsigned)w, (unsigned)h)) return NULL;
        sw = (unsigned)w; sh = (unsigned)h;
        jpeg = stbi_load_from_memory(bytes, (int)size, &w, &h, &n, 1);
        if (!jpeg || w != (int)sw || h != (int)sh || cover_cancelled()) { stbi_image_free(jpeg); return NULL; }
    }
    cover_reduce_t r = {.source_w = sw, .source_h = sh};
    cover_target(sw, sh, &r.width, &r.height,max_w,max_h);
    size_t count = (size_t)r.width * r.height;
    if(nearest)r.nearest=cover_work_calloc(count,1);
    else r.samples = cover_work_calloc(count, sizeof(*r.samples));
    if (!r.samples && !r.nearest) { stbi_image_free(jpeg); return NULL; }
    if (png) {
        s_png_remaining = ((size_t)sw * channels + 1) * sh;
        s_png_finished = false;
        pngle_t* decoder = pngle_new();
        if (decoder) {
            pngle_set_user_data(decoder, &r); pngle_set_draw_callback(decoder, cover_png_pixel); pngle_set_done_callback(decoder, cover_png_done);
            size_t pos = 0;
            while (pos < size) {
                if (cover_cancelled()) { r.invalid=true; break; }
                size_t length = size - pos < 1024 ? size - pos : 1024;
                int consumed = pngle_feed(decoder, bytes + pos, length);
                if (consumed <= 0 || r.invalid) break;
                pos += (size_t)consumed; cover_yield();
            }
            r.done = r.done && pos == size && s_png_remaining == 0 && s_png_finished;
            pngle_destroy(decoder);
        }
    } else {
        for (unsigned y = 0; y < sh; ++y) {
            if (!(y & 31) && cover_cancelled()) { r.invalid=true; break; }
            for (unsigned x = 0; x < sw; ++x) cover_reduce_pixel(&r, x, y, jpeg[(size_t)y * sw + x]);
            if (!(y & 31)) cover_yield();
        }
        stbi_image_free(jpeg); r.done = true;
    }
    uint8_t* result = NULL;
    if (r.done && !r.invalid && r.pixels == (uint64_t)sw * sh) {
        result = cover_alloc(count);
        if(result && nearest)memcpy(result,r.nearest,count);
        else if (result) for (size_t i = 0; i < count; ++i) {
            if (!r.samples[i].count) { free(result); result = NULL; break; }
            result[i] = (uint8_t)((r.samples[i].sum + r.samples[i].count / 2) / r.samples[i].count);
        }
    }
    cover_work_free(r.samples);
    cover_work_free(r.nearest);
    if (result) { *width = r.width; *height = r.height; }
    return result;
}
static uint8_t* cover_decode(const uint8_t* bytes,size_t size,unsigned* width,unsigned* height) {
    return cover_decode_target(bytes,size,width,height,COVER_W,COVER_H,false);
}
static uint64_t cover_mix(uint64_t state, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) { state = (state ^ (value & 255)) * UINT64_C(1099511628211); value >>= 8; }
    return state;
}
static uint64_t cover_stat(uint64_t state, const char* path) {
    struct stat s;
    if (stat(path, &s) != 0) return cover_mix(state, UINT64_MAX);
    state = cover_mix(state, (uint64_t)s.st_size);
    state = cover_mix(state, (uint64_t)s.st_mtime);
    return cover_mix(state, (uint64_t)s.st_ino);
}
static bool cover_sidecar(const char* path, const char* extension, char output[COVER_PATH]) {
    char name[16];snprintf(name,sizeof(name),"cover%s",extension);
    if(folio_wr_aux(path,name,output,COVER_PATH))return true;
    size_t length = strlen(path);
    const char* slash = strrchr(path, '/'); const char* dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash)) length = (size_t)(dot - path);
    size_t tail = strlen(extension);
    if (length + tail >= COVER_PATH) return false;
    memcpy(output, path, length); memcpy(output + length, extension, tail + 1); return true;
}
static uint64_t cover_stamp(const char* path) {
    uint64_t stamp = cover_stat(UINT64_C(1469598103934665603), path);
    const char* extensions[] = {".jpg", ".jpeg", ".png"};
    char sidecar[COVER_PATH];
    for (unsigned i = 0; i < 3; ++i) if (cover_sidecar(path, extensions[i], sidecar)) stamp = cover_stat(stamp, sidecar);
    return stamp;
}
static uint8_t* cover_read(const char* path, size_t* size) {
    *size = 0; struct stat s;
    if (stat(path, &s) != 0 || !S_ISREG(s.st_mode) || s.st_size <= 0 || (uint64_t)s.st_size > COVER_INPUT_MAX) return NULL;
    FILE* file = fopen(path, "rb"); if (!file) return NULL;
    size_t length = (size_t)s.st_size;
    uint8_t* bytes = cover_alloc(length);
    bool ok = bytes!=NULL; size_t read=0;
    while(ok && read<length) {
        size_t count=length-read<65536?length-read:65536;
        if(cover_cancelled() || fread(bytes+read,1,count,file)!=count)ok=false;
        else read+=count;
    }
    ok=ok && fgetc(file)==EOF && !ferror(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) { free(bytes); return NULL; }
    *size = length; return bytes;
}
uint8_t* folio_cover_decode_legacy_jpeg_file(const char* path, unsigned max_w, unsigned max_h,
                                           unsigned* width, unsigned* height, size_t* peak_bytes,
                                           bool (*cancelled)(void*), void* cancel_context) {
    if (peak_bytes) *peak_bytes = 0;
    if (!path || !width || !height || !max_w || !max_h || max_w > 1216 || max_h > 1216 || s_work_live) return NULL;
    size_t size = 0;
    s_legacy_psram_only = true;
    s_legacy_cancel = cancelled; s_legacy_cancel_context = cancel_context;
    uint8_t* bytes = cover_read(path, &size);
    if (!bytes) { s_legacy_psram_only = false; s_legacy_cancel = NULL; s_legacy_cancel_context = NULL; return NULL; }
    s_work_peak = 0;
    uint8_t* pixels = NULL;
    if (size >= 2 && bytes[0] == 255 && bytes[1] == 216)
        pixels = cover_decode_target(bytes, size, width, height, max_w, max_h, true);
    /* Conservative upper bound: the returned gray8 overlaps the work buffer. */
    if (peak_bytes) *peak_bytes = size + s_work_peak + (pixels ? (size_t)*width * *height : 0);
    free(bytes);
    s_legacy_psram_only = false; s_legacy_cancel = NULL; s_legacy_cancel_context = NULL;
    return pixels;
}
bool folio_cover_validate_file(const char* path) {
    if (!path) return false;
    size_t size=0; unsigned width=0, height=0;
    uint8_t* bytes=cover_read(path,&size);
    uint8_t* pixels=bytes?cover_decode(bytes,size,&width,&height):NULL;
    bool valid=pixels!=NULL;
    free(pixels); free(bytes);
    return valid;
}
static cover_image_t* cover_store(uint8_t* pixels, unsigned w, unsigned h) {
    cover_image_t* slot = &s_images[0];
    for (unsigned i = 0; i < COVER_IMAGES; ++i) {
        cover_image_t* image = &s_images[i];
        if (image->pixels && image->width == w && image->height == h && !memcmp(image->pixels,pixels,(size_t)w*h)) {
            free(pixels); image->age = ++s_age; return image;
        }
        if (!image->pixels || (slot->pixels && image->age < slot->age)) slot = image;
    }
    for (unsigned i = 0; i < COVER_BOOKS; ++i) if (s_books[i].image == slot) s_books[i].used = false;
    free(slot->pixels); *slot = (cover_image_t){pixels, w, h, ++s_age}; return slot;
}
static cover_image_t* cover_load(const char* path) {
    size_t size = 0; uint8_t* source = NULL; uint8_t* pixels = NULL; unsigned w = 0, h = 0;
    const char* dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".epub")) {
        char error[96];
        if (folio_epub_cover(path, &source, &size, error, sizeof(error))) pixels = cover_decode(source, size, &w, &h);
        free(source);
    }
    const char* extensions[] = {".jpg", ".jpeg", ".png"}; char sidecar[COVER_PATH];
    for (unsigned i = 0; !pixels && i < 3; ++i) if (cover_sidecar(path, extensions[i], sidecar)) {
        source = cover_read(sidecar, &size);
        if (source) pixels = cover_decode(source, size, &w, &h);
        free(source);
    }
    return pixels ? cover_store(pixels, w, h) : NULL;
}
void folio_cover_clear_cache(void) {
    for (unsigned i = 0; i < COVER_IMAGES; ++i) free(s_images[i].pixels);
    memset(s_images, 0, sizeof(s_images)); memset(s_books, 0, sizeof(s_books)); s_age = 0;
}
static bool cover_rect_valid(const char* path,uint8_t* framebuffer,EpdRect rect) {
    return path && *path && strlen(path)<COVER_PATH && framebuffer && rect.width>0 && rect.height>0 && rect.x>=0 && rect.y>=0 &&
        (int64_t)rect.x+rect.width<=epd_rotated_display_width() && (int64_t)rect.y+rect.height<=epd_rotated_display_height();
}
static void cover_paint(const cover_image_t* image,uint8_t* framebuffer,EpdRect rect);
bool folio_cover_draw_image(const char* path,uint8_t* framebuffer,EpdRect rect) {
    if(!cover_rect_valid(path,framebuffer,rect))return false;
    const char* extension=strrchr(path,'.');
    if(!extension || (strcasecmp(extension,".png") && strcasecmp(extension,".jpg") && strcasecmp(extension,".jpeg")))return false;
    size_t size=0;unsigned w=0,h=0;
    uint8_t* source=cover_read(path,&size);
    uint8_t* pixels=source?cover_decode_target(source,size,&w,&h,(unsigned)rect.width,(unsigned)rect.height,true):NULL;
    free(source);
    if(!pixels)return false;
    cover_image_t image={.pixels=pixels,.width=w,.height=h};
    cover_paint(&image,framebuffer,rect);free(pixels);return true;
}
bool folio_cover_draw(const char* path, uint8_t* framebuffer, EpdRect rect) {
    if(!cover_rect_valid(path,framebuffer,rect))return false;
    uint64_t stamp = cover_stamp(path);
    cover_book_t* book = NULL; cover_book_t* oldest = &s_books[0];
    for (unsigned i = 0; i < COVER_BOOKS; ++i) {
        cover_book_t* candidate = &s_books[i];
        if (candidate->used && !strcmp(candidate->path, path)) { book = candidate; break; }
        if (!candidate->used || (oldest->used && candidate->age < oldest->age)) oldest = candidate;
    }
    if (!book || book->stamp != stamp) {
        if (!book) book = oldest;
        cover_image_t* image = cover_load(path);
        *book = (cover_book_t){.stamp = stamp, .image = image, .used = true};
        memcpy(book->path, path, strlen(path) + 1);
    }
    book->age = ++s_age;
    cover_image_t* image = book->image;
    if (!image) return false;
    image->age = s_age;
    cover_paint(image,framebuffer,rect);
    return true;
}
static void cover_paint(const cover_image_t* image,uint8_t* framebuffer,EpdRect rect) {
    unsigned w = (unsigned)rect.width, h = (unsigned)((uint64_t)image->height * w / image->width);
    if (h > (unsigned)rect.height) { h = (unsigned)rect.height; w = (unsigned)((uint64_t)image->width * h / image->height); }
    if (!w) w = 1;
    if (!h) h = 1;
    int left = rect.x + (rect.width - (int)w) / 2, top = rect.y + (rect.height - (int)h) / 2;
    static const uint8_t bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    for (int y = rect.y; y < rect.y + rect.height; ++y) for (int x = rect.x; x < rect.x + rect.width; ++x) {
        unsigned gray = 255;
        if (x >= left && x < left + (int)w && y >= top && y < top + (int)h) {
            unsigned sx = (unsigned)((uint64_t)(x - left) * image->width / w);
            unsigned sy = (unsigned)((uint64_t)(y - top) * image->height / h);
            gray = image->pixels[(size_t)sy * image->width + sx];
            unsigned level = gray / 17, remainder = gray % 17;
            if (level < 15 && remainder * 16 > (unsigned)bayer[y & 3][x & 3] * 17 + 8) ++level;
            gray = level * 17;
        }
        epd_draw_pixel(x, y, (uint8_t)gray, framebuffer);
    }
}

// GCC reports disabled stb static API declarations at end of translation unit.
// Keep this exception local to this decoder wrapper, not the application build.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
