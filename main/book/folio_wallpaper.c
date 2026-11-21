#include "folio_wallpaper.h"
#include "folio_wallpaper_limits.h"
#include "folio_cover.h"
#include "esp_heap_caps.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define WALL_IO_SIZE 8192u
#define WALL_JPEG_POOL 8192u
#define WALL_SYSTEM_RESERVE (384u * 1024u)
extern bool folio_io_cancelled(void);

typedef struct {
    FILE* file;
    struct stat fd_original, path_original;
    size_t size, pos, available, cursor;
    uint8_t* input;
    const char* failure;
    uint64_t started, yielded;
    size_t live, peak, legacy_peak;
    unsigned source_w, source_h, channels, orientation;
    bool png, legacy, exif_seen, png_done, inflate_done;
    uint64_t inflate_remaining;
    folio_wallpaper_image_t image;
    unsigned sw, sh, dw, dh, crop_w, crop_h, left, top, next_x, next_y;
    bool fill;
    uint64_t* horizontal;
    uint64_t* accumulated;
    uint8_t* stripe;
    unsigned jpeg_x, jpeg_y;
    size_t jpeg_eoi;
} wall_context_t;
static wall_context_t* s_wall_context;
static size_t s_wall_last_peak, s_wall_last_legacy_peak, s_wall_last_live;
typedef union { size_t size; max_align_t align; } wall_allocation_t;

static uint64_t wall_now(void) {
#ifdef FOLIO_WALLPAPER_NOW_MS
    return FOLIO_WALLPAPER_NOW_MS();
#elif defined(ESP_PLATFORM)
    return (uint64_t)esp_timer_get_time() / 1000u;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}
static bool wall_fail(wall_context_t* c, const char* error) {
    if (!c->failure) c->failure = error;
    return false;
}
static bool wall_poll(wall_context_t* c) {
    if (c->failure) return false;
    if (folio_io_cancelled()) return wall_fail(c, "[cancelled] 已取消壁纸处理。");
    uint64_t now = wall_now();
    if (now - c->started >= FOLIO_WALLPAPER_TIMEOUT_MS)
        return wall_fail(c, "[timeout] 图片处理超过60秒，请缩小后重试。");
#ifdef ESP_PLATFORM
    if (now - c->yielded >= 16) { vTaskDelay(1); c->yielded = now; }
#endif
    return true;
}
static void* wall_alloc(size_t bytes) {
    wall_context_t* c = s_wall_context;
    if (!c || !bytes || bytes > SIZE_MAX - sizeof(wall_allocation_t)) return NULL;
    size_t total = bytes + sizeof(wall_allocation_t);
    if (total > FOLIO_WALLPAPER_WORK_MAX - c->live) {
        wall_fail(c, "[memory] 壁纸工作内存不足。"); return NULL;
    }
#ifdef ESP_PLATFORM
    size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (available < WALL_SYSTEM_RESERVE || total > available - WALL_SYSTEM_RESERVE) {
        wall_fail(c, "[memory] 可用PSRAM不足，请稍后重试。"); return NULL;
    }
#endif
    wall_allocation_t* block = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!block) { wall_fail(c, "[memory] 无法分配壁纸PSRAM。"); return NULL; }
    block->size = total; c->live += total;
    if (c->live > c->peak) c->peak = c->live;
    memset(block + 1, 0, bytes);
    return block + 1;
}
static void wall_free(void* pointer) {
    if (!pointer) return;
    wall_allocation_t* block = (wall_allocation_t*)pointer - 1;
    if (s_wall_context) s_wall_context->live -= block->size;
    free(block);
}
static void* wall_calloc(size_t count, size_t bytes) {
    if (count && bytes > SIZE_MAX / count) return NULL;
    return wall_alloc(count * bytes);
}
static bool wall_png_output(size_t bytes) {
    wall_context_t* c = s_wall_context;
    if (!wall_poll(c)) return false;
    if (bytes > c->inflate_remaining) return wall_fail(c, "[corrupt] PNG解压尺寸不符。");
    c->inflate_remaining -= bytes;
    return true;
}

/* Private, separately prefixed PNG inflater. Never interpose on EPUB ROM miniz. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define mz_crc32 folio_wall_mz_crc32
#define mz_adler32 folio_wall_mz_adler32
#define mz_free folio_wall_mz_free
#define tinfl_decompress folio_wall_tinfl_decompress
#define tinfl_decompress_mem_to_heap folio_wall_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem folio_wall_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback folio_wall_tinfl_decompress_mem_to_callback
#define MINIZ_NO_MALLOC
#include "vendor/pngle/miniz.c"
#define pngle_new folio_wall_pngle_new
#define pngle_reset folio_wall_pngle_reset
#define pngle_destroy folio_wall_pngle_destroy
#define pngle_error folio_wall_pngle_error
#define pngle_feed folio_wall_pngle_feed
#define pngle_get_width folio_wall_pngle_get_width
#define pngle_get_height folio_wall_pngle_get_height
#define pngle_get_background_color folio_wall_pngle_get_background_color
#define pngle_set_init_callback folio_wall_pngle_set_init_callback
#define pngle_set_draw_callback folio_wall_pngle_set_draw_callback
#define pngle_set_done_callback folio_wall_pngle_set_done_callback
#define pngle_set_display_gamma folio_wall_pngle_set_display_gamma
#define pngle_set_user_data folio_wall_pngle_set_user_data
#define pngle_get_user_data folio_wall_pngle_get_user_data
#define pngle_get_ihdr folio_wall_pngle_get_ihdr
#define PNGLE_T_SIZE FOLIO_WALL_PNGLE_T_SIZE
#define calloc wall_calloc
#define free wall_free
#define PNGLE_NO_GAMMA_CORRECTION
#define PNGLE_OUTPUT_CHECK wall_png_output
#define PNGLE_INFLATE_STATUS(status) do { if ((status) == TINFL_STATUS_DONE) s_wall_context->inflate_done = true; } while (0)
#include "vendor/pngle/pngle.c"
#undef calloc
#undef free
#define jd_prepare folio_wall_jd_prepare
#define jd_decomp folio_wall_jd_decomp
#include "vendor/tjpgd/tjpgd.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static uint16_t wall_be16(const uint8_t* p) { return (uint16_t)((unsigned)p[0] * 256u + p[1]); }
static uint32_t wall_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static bool wall_read(wall_context_t* c, void* output, size_t length) {
    if (length > c->size - c->pos) return wall_fail(c, "[corrupt] 图片文件不完整。");
    uint8_t* dst = output;
    while (length) {
        if (c->cursor == c->available) {
            if (!wall_poll(c)) return false;
            size_t wanted = c->size - c->pos;
            if (wanted > WALL_IO_SIZE) wanted = WALL_IO_SIZE;
            c->available = fread(c->input, 1, wanted, c->file); c->cursor = 0;
            if (c->available != wanted) return wall_fail(c, "[io] 图片读取失败或已变化。");
        }
        size_t n = c->available - c->cursor;
        if (n > length) n = length;
        if (dst) { memcpy(dst, c->input + c->cursor, n); dst += n; }
        c->cursor += n; c->pos += n; length -= n;
    }
    return true;
}
static bool wall_seek(wall_context_t* c, size_t pos) {
    if (!wall_poll(c) || pos > c->size) return false;
    if (fseek(c->file, (long)pos, SEEK_SET)) return wall_fail(c, "[io] 图片定位失败。");
    c->pos = pos; c->cursor = c->available = 0;
    return true;
}
static int wall_byte(wall_context_t* c) {
    uint8_t b;
    return wall_read(c, &b, 1) ? b : -1;
}
static bool wall_dimensions(wall_context_t* c, unsigned w, unsigned h) {
    if (!w || !h) return wall_fail(c, "[corrupt] 图片尺寸无效。");
    if (w > FOLIO_WALLPAPER_DIM_MAX || h > FOLIO_WALLPAPER_DIM_MAX)
        return wall_fail(c, "[dimensions] 图片单边不能超过8192像素。");
    if ((uint64_t)w * h > FOLIO_WALLPAPER_PIXEL_MAX)
        return wall_fail(c, "[pixels] 图片不能超过3200万像素。");
    c->source_w = w; c->source_h = h;
    return true;
}
static bool wall_png_header(wall_context_t* c) {
    uint8_t header[13];
    if (!wall_seek(c, 8)) return false;
    bool ihdr = false, idat = false, after_idat = false, palette = false;
    unsigned color = 0;
    while (c->pos < c->size) {
        if (!wall_poll(c) || !wall_read(c, header, 8)) return false;
        uint32_t length = wall_be32(header);
        uint8_t* type = header + 4;
        if (c->size - c->pos < 4 || length > c->size - c->pos - 4)
            return wall_fail(c, "[corrupt] PNG区块越界。");
        for (unsigned i = 0; i < 4; ++i)
            if (!((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z')))
                return wall_fail(c, "[corrupt] PNG区块名称无效。");
        if (type[2] & 32) return wall_fail(c, "[corrupt] PNG保留位无效。");
        size_t end = c->pos + length + 4;
        if (!memcmp(type, "IHDR", 4)) {
            if (ihdr || c->pos != 16 || length != 13 || !wall_read(c, header, 13))
                return wall_fail(c, "[corrupt] PNG文件头无效。");
            if (!wall_dimensions(c, wall_be32(header), wall_be32(header + 4))) return false;
            if (header[8] != 8 || header[10] || header[11] || header[12])
                return wall_fail(c, "[format] 请使用8位非交错PNG。");
            color = header[9];
            switch (color) {
                case 0: case 3: c->channels = 1; break;
                case 2: c->channels = 3; break;
                case 4: c->channels = 2; break;
                case 6: c->channels = 4; break;
                default: return wall_fail(c, "[format] 不支持此PNG颜色格式。");
            }
            ihdr = true;
        } else {
            if (!ihdr) return wall_fail(c, "[corrupt] PNG缺少文件头。");
            if (!memcmp(type, "acTL", 4) || !memcmp(type, "fcTL", 4) || !memcmp(type, "fdAT", 4))
                return wall_fail(c, "[format] 不支持动画PNG。");
            if (!memcmp(type, "IDAT", 4)) {
                if (after_idat || !length || (color == 3 && !palette))
                    return wall_fail(c, "[corrupt] PNG数据区块无效。");
                idat = true;
            } else {
                if (idat) after_idat = true;
                if (!memcmp(type, "PLTE", 4)) {
                    if (palette || idat || color == 0 || color == 4 || !length || length > 768 || length % 3)
                        return wall_fail(c, "[corrupt] PNG调色板无效。");
                    palette = true;
                } else if (!memcmp(type, "IEND", 4)) {
                    if (length || !idat || end != c->size) return wall_fail(c, "[corrupt] PNG结束标记无效。");
                    return true;
                } else if (!(type[0] & 32)) return wall_fail(c, "[format] 不支持此PNG关键区块。");
                else if (!memcmp(type, "gAMA", 4) && length != 4) return wall_fail(c, "[corrupt] PNG gAMA区块无效。");
            }
        }
        if (!wall_seek(c, end)) return false;
    }
    return wall_fail(c, "[corrupt] PNG缺少结束标记。");
}
static uint16_t wall_tiff16(const uint8_t* p, bool little) {
    return little ? (uint16_t)((unsigned)p[1] * 256u + p[0]) : wall_be16(p);
}
static uint32_t wall_tiff32(const uint8_t* p, bool little) {
    return little ? (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0] : wall_be32(p);
}
static bool wall_exif(wall_context_t* c, size_t start, size_t length) {
    uint8_t b[14];
    if (length < 6) return true;
    if (!wall_read(c, b, 6)) return false;
    if (memcmp(b, "Exif\0\0", 6)) return true;
    if (length < 14 || !wall_read(c, b, 8)) return wall_fail(c, "[corrupt] EXIF文件头不完整。");
    bool little = b[0] == 'I' && b[1] == 'I';
    if ((!little && (b[0] != 'M' || b[1] != 'M')) || wall_tiff16(b + 2, little) != 42)
        return wall_fail(c, "[corrupt] EXIF字节序无效。");
    uint32_t offset = wall_tiff32(b + 4, little);
    size_t tiff = start + 6, available = length - 6;
    if (offset < 8 || offset > available - 2 || !wall_seek(c, tiff + offset) || !wall_read(c, b, 2))
        return wall_fail(c, "[corrupt] EXIF目录越界。");
    unsigned entries = wall_tiff16(b, little);
    if ((uint64_t)offset + 2u + (uint64_t)entries * 12u + 4u > available)
        return wall_fail(c, "[corrupt] EXIF目录不完整。");
    for (unsigned i = 0; i < entries; ++i) {
        if (!wall_poll(c) || !wall_read(c, b, 12)) return false;
        if (wall_tiff16(b, little) != 0x112) continue;
        unsigned orientation = wall_tiff16(b + 8, little);
        if (wall_tiff16(b + 2, little) != 3 || wall_tiff32(b + 4, little) != 1 || orientation < 1 || orientation > 8)
            return wall_fail(c, "[corrupt] EXIF方向无效。");
        if (c->exif_seen && orientation != c->orientation) return wall_fail(c, "[corrupt] EXIF方向冲突。");
        c->orientation = orientation; c->exif_seen = true;
    }
    return true;
}
static bool wall_jpeg_tables(wall_context_t* c, unsigned marker, const uint8_t* p, size_t n,
                             unsigned* quant, unsigned* huffman) {
    size_t pos = 0;
    while (pos < n) {
        unsigned spec = p[pos++];
        if (marker == 0xDB) {
            if (spec > 3 || n - pos < 64) return wall_fail(c, "[corrupt] JPEG量化表无效。");
            if (*quant & (1u << spec)) c->legacy = true;
            *quant |= 1u << spec;
            for (unsigned i = 0; i < 64; ++i) if (!p[pos + i]) return wall_fail(c, "[corrupt] JPEG零量化值。");
            pos += 64;
        } else {
            if (spec & 0xEEu || n - pos < 16) return wall_fail(c, "[format] 不支持此JPEG霍夫曼表。");
            unsigned id = (spec >> 4) * 2 + (spec & 1), count = 0;
            int unused = 1;
            for (unsigned i = 0; i < 16; ++i) {
                count += p[pos + i]; unused = unused * 2 - p[pos + i];
                if (unused <= 0) return wall_fail(c, "[corrupt] JPEG霍夫曼码超额。");
            }
            pos += 16;
            if (!count || count > 256 || count > n - pos) return wall_fail(c, "[corrupt] JPEG霍夫曼表越界。");
            if (*huffman & (1u << id)) c->legacy = true;
            *huffman |= 1u << id;
            bool seen[256] = {false};
            for (unsigned i = 0; i < count; ++i) {
                unsigned v = p[pos + i];
                if (seen[v] || ((spec & 16) ? ((v & 15) > 10 || (!(v & 15) && v != 0 && v != 0xF0)) : v > 11))
                    return wall_fail(c, "[corrupt] JPEG霍夫曼符号无效。");
                seen[v] = true;
            }
            pos += count;
        }
    }
    return pos == n;
}
static int wall_jpeg_marker_prefix(wall_context_t* c, bool entropy, size_t* bytes) {
    if (!entropy) return wall_byte(c);
    /* Skip entropy runs a buffer at a time, not one timer/fread call per byte. */
    while (c->pos < c->size) {
        if (c->cursor == c->available) {
            int b = wall_byte(c);
            if (b < 0 || b == 255) return b;
            ++*bytes;
        }
        size_t available = c->available - c->cursor;
        const uint8_t* p = c->input + c->cursor;
        const uint8_t* marker = memchr(p, 255, available);
        size_t run = marker ? (size_t)(marker - p) : available;
        c->cursor += run; c->pos += run; *bytes += run;
        if (marker) { ++c->cursor; ++c->pos; return 255; }
    }
    wall_fail(c, "[corrupt] JPEG缺少结束标记。");
    return -1;
}
static bool wall_jpeg_header(wall_context_t* c) {
    uint8_t segment[512];
    bool frame = false, entropy = false;
    unsigned components = 0, ids[4] = {0}, qids[4] = {0}, scans = 0;
    unsigned quant = 0, huffman = 0, interval = 0, restart_count = 0, mx = 8, my = 8;
    size_t entropy_bytes = 0;
    if (!wall_seek(c, 2)) return false;
    while (c->pos < c->size) {
        if (!wall_poll(c)) return false;
        int b = wall_jpeg_marker_prefix(c, entropy, &entropy_bytes);
        if (b < 0) return false;
        if (b != 255) return wall_fail(c, "[corrupt] JPEG标记无效。");
        size_t marker_pos = c->pos - 1;
        do { b = wall_byte(c); } while (b == 255);
        if (b < 0) return false;
        unsigned marker = (unsigned)b;
        if (entropy && marker == 0) { ++entropy_bytes; continue; }
        if (entropy && marker >= 0xD0 && marker <= 0xD7) {
            if (!interval || marker != 0xD0 + restart_count % 8) return wall_fail(c, "[corrupt] JPEG重启标记无效。");
            ++restart_count; continue;
        }
        if (entropy && !entropy_bytes) return wall_fail(c, "[corrupt] JPEG扫描为空。");
        entropy = false;
        if (marker == 0xD9) {
            if (!frame || !scans || c->pos != c->size) return wall_fail(c, "[corrupt] JPEG结束标记无效。");
            if (!c->legacy) {
                uint64_t mcus = ((c->source_w + mx - 1) / mx) * (uint64_t)((c->source_h + my - 1) / my);
                if (restart_count != (interval ? (mcus - 1) / interval : 0)) return wall_fail(c, "[corrupt] JPEG重启标记缺失。");
            }
            c->jpeg_eoi = marker_pos;
            return true;
        }
        if (!marker || marker == 0xD8 || marker == 1 || (marker >= 0xD0 && marker <= 0xD7))
            return wall_fail(c, "[corrupt] JPEG标记顺序无效。");
        if (!wall_read(c, segment, 2)) return false;
        size_t length = wall_be16(segment);
        if (length < 2 || length - 2 > c->size - c->pos) return wall_fail(c, "[corrupt] JPEG区段越界。");
        length -= 2;
        size_t start = c->pos, end = start + length;
        if (scans) c->legacy = true;
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (frame || (marker != 0xC0 && marker != 0xC1 && marker != 0xC2) || length < 9 || length > sizeof(segment))
                return wall_fail(c, "[format] 不支持此JPEG编码。");
            if (!wall_read(c, segment, length)) return false;
            components = segment[5];
            if (segment[0] != 8 || !components || components > 4 || length != 6u + 3u * components)
                return wall_fail(c, "[format] 仅支持8位JPEG。");
            if (!wall_dimensions(c, wall_be16(segment + 3), wall_be16(segment + 1))) return false;
            if (marker != 0xC0 || (components != 1 && components != 3)) c->legacy = true;
            for (unsigned i = 0; i < components; ++i) {
                ids[i] = segment[6 + 3 * i]; qids[i] = segment[8 + 3 * i];
                for (unsigned j = 0; j < i; ++j) if (ids[j] == ids[i]) return wall_fail(c, "[corrupt] JPEG分量重复。");
                unsigned sampling = segment[7 + 3 * i];
                if (!sampling || !(sampling & 15) || (sampling & 15) > 4 || (sampling >> 4) > 4 || qids[i] > 3)
                    return wall_fail(c, "[corrupt] JPEG采样无效。");
                if (i == 0) {
                    if (sampling != 0x11 && sampling != 0x21 && sampling != 0x22) c->legacy = true;
                    mx = (sampling >> 4) * 8; my = (sampling & 15) * 8;
                } else if (sampling != 0x11) c->legacy = true;
                if (components == 3 && ids[i] != i + 1) c->legacy = true;
            }
            frame = true;
        } else if (marker == 0xDA) {
            if (!frame || length < 6 || length > sizeof(segment) || !wall_read(c, segment, length))
                return wall_fail(c, "[corrupt] JPEG扫描头无效。");
            unsigned n = segment[0], mask = 0;
            if (!n || n > components || length != 4u + 2u * n) return wall_fail(c, "[corrupt] JPEG扫描分量无效。");
            for (unsigned i = 0; i < n; ++i) {
                unsigned j = 0;
                while (j < components && ids[j] != segment[1 + 2 * i]) ++j;
                if (j == components || (mask & (1u << j))) return wall_fail(c, "[corrupt] JPEG扫描分量重复。");
                mask |= 1u << j;
                if (j != i || segment[2 + 2 * i] != (i ? 0x11 : 0x00)) c->legacy = true;
            }
            if (n != components || segment[length - 3] != 0 || segment[length - 2] != 63 || segment[length - 1]) c->legacy = true;
            if (!c->legacy) {
                for (unsigned i = 0; i < components; ++i)
                    if (!(quant & (1u << qids[i])) || (huffman & (i ? 10u : 5u)) != (i ? 10u : 5u))
                        return wall_fail(c, "[corrupt] JPEG缺少编码表。");
            }
            if (++scans > 256) return wall_fail(c, "[format] JPEG扫描次数过多。");
            entropy = true; entropy_bytes = 0; restart_count = 0;
        } else if (marker == 0xDB || marker == 0xC4) {
            if (length > sizeof(segment)) c->legacy = true;
            else if (!c->legacy && (!wall_read(c, segment, length) || !wall_jpeg_tables(c, marker, segment, length, &quant, &huffman))) return false;
        } else if (marker == 0xDD) {
            if (length != 2 || !wall_read(c, segment, 2)) return wall_fail(c, "[corrupt] JPEG重启间隔无效。");
            interval = wall_be16(segment);
        } else if (marker == 0xE1) {
            if (!wall_exif(c, start, length)) return false;
        } else if (marker == 0xEE && length >= 12) {
            if (!wall_read(c, segment, 12)) return false;
            if (!memcmp(segment, "Adobe", 5) && segment[11] != 1) c->legacy = true;
        } else if (!((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE))
            return wall_fail(c, "[format] 不支持此JPEG标记。");
        /* Preserve prefetch during entropy; otherwise bounded seek skips metadata. */
        if (c->pos != end && !wall_seek(c, end)) return false;
    }
    return wall_fail(c, "[corrupt] JPEG缺少结束标记。");
}
static void wall_fit(wall_context_t* c, unsigned sw, unsigned sh) {
    bool rotated = c->orientation >= 5;
    unsigned ow = rotated ? sh : sw, oh = rotated ? sw : sh;
    unsigned w = c->image.width, h = (unsigned)((uint64_t)oh * w / ow);
    if (h > c->image.height) { h = c->image.height; w = (unsigned)((uint64_t)ow * h / oh); }
    if (!w) w = 1;
    if (!h) h = 1;
    c->crop_w = sw;
    c->crop_h = sh;
    if (c->fill) {
        w = c->image.width;
        h = c->image.height;
        unsigned tw = rotated ? h : w, th = rotated ? w : h;
        if ((uint64_t)sw * th > (uint64_t)sh * tw)
            c->crop_w = (unsigned)((uint64_t)sh * tw / th);
        else
            c->crop_h = (unsigned)((uint64_t)sw * th / tw);
        if (!c->crop_w) c->crop_w = 1;
        if (!c->crop_h) c->crop_h = 1;
    }
    c->left = (c->image.width - w) / 2; c->top = (c->image.height - h) / 2;
    c->dw = rotated ? h : w; c->dh = rotated ? w : h;
    c->sw = sw; c->sh = sh;
}
static void wall_pixel(wall_context_t* c, unsigned x, unsigned y, unsigned gray) {
    unsigned ox = x, oy = y;
    switch (c->orientation) {
        case 2: ox = c->dw - 1 - x; break;
        case 3: ox = c->dw - 1 - x; oy = c->dh - 1 - y; break;
        case 4: oy = c->dh - 1 - y; break;
        case 5: ox = y; oy = x; break;
        case 6: ox = c->dh - 1 - y; oy = x; break;
        case 7: ox = c->dh - 1 - y; oy = c->dw - 1 - x; break;
        case 8: ox = y; oy = c->dw - 1 - x; break;
        default: break;
    }
    ox += c->left; oy += c->top;
    static const uint8_t bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    unsigned level = gray / 17, remainder = gray % 17;
    if (level < 15 && remainder * 16 > (unsigned)bayer[oy & 3][ox & 3] * 17 + 8) ++level;
    uint8_t* p = &c->image.pixels[(size_t)oy * c->image.stride + ox / 2];
    if (ox & 1) *p = (uint8_t)((*p & 0xF0u) | level);
    else *p = (uint8_t)((*p & 0x0Fu) | (level << 4));
}
static bool wall_reduce_begin(wall_context_t* c, unsigned sw, unsigned sh) {
    wall_fit(c, sw, sh);
    c->horizontal = wall_calloc(c->dw, sizeof(uint64_t));
    c->accumulated = wall_calloc(c->dw, sizeof(uint64_t));
    return c->horizontal && c->accumulated;
}
static bool wall_reduce_pixel(wall_context_t* c, unsigned x, unsigned y, unsigned gray) {
    if (c->failure) return false;
    if (x != c->next_x || y != c->next_y || x >= c->sw || y >= c->sh)
        return wall_fail(c, "[corrupt] 解码像素顺序或尺寸无效。");
    /* Dimensions are prebounded to 8192/1216: these grid coordinates fit
     * uint32, avoiding millions of software uint64 divisions on ESP32-S3. */
    if (++c->next_x == c->sw) { c->next_x = 0; ++c->next_y; }
    unsigned left = (c->sw - c->crop_w) / 2, top = (c->sh - c->crop_h) / 2;
    if (x < left || x - left >= c->crop_w || y < top || y - top >= c->crop_h) return true;
    x -= left;
    y -= top;
    unsigned start = x * c->dw, end = start + c->dw;
    while (start < end) {
        unsigned dx = start / c->crop_w;
        unsigned stop = (dx + 1) * c->crop_w;
        if (stop > end) stop = end;
        c->horizontal[dx] += gray * (stop - start); start = stop;
    }
    if (x + 1 < c->crop_w) return true;
    start = y * c->dh; end = start + c->dh;
    while (start < end) {
        unsigned dy = start / c->crop_h;
        unsigned bottom = (dy + 1) * c->crop_h, stop = bottom < end ? bottom : end;
        uint64_t weight = stop - start, divisor = (uint64_t)c->crop_w * c->crop_h;
        for (unsigned dx = 0; dx < c->dw; ++dx) {
            c->accumulated[dx] += c->horizontal[dx] * weight;
            if (stop == bottom) {
                unsigned value = (unsigned)((c->accumulated[dx] + divisor / 2) / divisor);
                wall_pixel(c, dx, dy, value); c->accumulated[dx] = 0;
            }
        }
        start = stop;
    }
    memset(c->horizontal, 0, c->dw * sizeof(uint64_t));
    return (y & 15) || wall_poll(c);
}
static void wall_png_draw(pngle_t* png, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    wall_context_t* c = pngle_get_user_data(png);
    if (w != 1 || h != 1) { wall_fail(c, "[format] 不支持交错PNG。"); return; }
    unsigned gray = (77u * rgba[0] + 150u * rgba[1] + 29u * rgba[2] + 128u) >> 8;
    gray = (gray * rgba[3] + 255u * (255u - rgba[3]) + 127u) / 255u;
    (void)wall_reduce_pixel(c, x, y, gray);
}
static void wall_png_done(pngle_t* png) { ((wall_context_t*)pngle_get_user_data(png))->png_done = true; }
/* miniz can retain a long match after consuming the last IDAT byte. pngle
 * otherwise advances to CRC/IEND before draining that pending output. Drain
 * without inventing input bytes; a truncated zlib trailer still cannot DONE. */
static bool wall_png_drain(wall_context_t* c, pngle_t* png) {
    uint8_t empty = 0;
    while (!c->inflate_done && png->next_out && wall_poll(c)) {
        size_t in = 0, out = png->avail_out;
        tinfl_status status = tinfl_decompress(&png->inflator, &empty, &in, png->lz_buf,
                                               png->next_out, &out,
                                               TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (status < TINFL_STATUS_DONE) return wall_fail(c, "[corrupt] PNG压缩流损坏。");
        png->next_out += out; png->avail_out -= out;
        if (status == TINFL_STATUS_DONE) c->inflate_done = true;
        if (c->inflate_done || !png->avail_out) {
            if (pngle_on_data(png, png->lz_buf, (int)(TINFL_LZ_DICT_SIZE - png->avail_out)) < 0)
                return wall_fail(c, "[corrupt] PNG解压尺寸不符。");
            png->next_out = png->lz_buf; png->avail_out = TINFL_LZ_DICT_SIZE;
        }
        if (status != TINFL_STATUS_HAS_MORE_OUTPUT) break;
    }
    return !c->failure;
}
static bool wall_decode_png(wall_context_t* c) {
    if (!wall_seek(c, 0) || !wall_reduce_begin(c, c->source_w, c->source_h)) return false;
    c->inflate_remaining = ((uint64_t)c->source_w * c->channels + 1) * c->source_h;
    pngle_t* png = pngle_new();
    uint8_t* feed = wall_alloc(WALL_IO_SIZE);
    if (!png || !feed) { pngle_destroy(png); wall_free(feed); return false; }
    pngle_set_user_data(png, c); pngle_set_draw_callback(png, wall_png_draw); pngle_set_done_callback(png, wall_png_done);
    size_t held = 0;
    while (wall_poll(c) && (c->pos < c->size || held)) {
        size_t add = c->size - c->pos;
        if (add > WALL_IO_SIZE - held) add = WALL_IO_SIZE - held;
        if (add && !wall_read(c, feed + held, add)) break;
        held += add;
        int consumed = pngle_feed(png, feed, held);
        if (consumed < 0 || (size_t)consumed > held) { wall_fail(c, "[corrupt] PNG数据或校验损坏。"); break; }
        if (!consumed && (!add || held == WALL_IO_SIZE)) { wall_fail(c, "[corrupt] PNG数据不完整。"); break; }
        held -= (size_t)consumed;
        if (held) memmove(feed, feed + consumed, held);
    }
    if (!c->failure) (void)wall_png_drain(c, png);
    bool okay = !c->failure && c->png_done && c->inflate_done && !c->inflate_remaining && c->next_y == c->sh && !c->next_x;
#ifdef FOLIO_WALLPAPER_TEST_DIAGNOSTICS
    if (!okay) fprintf(stderr, "png status done=%u inflate=%u remain=%llu row=%u/%u x=%u pos=%zu held=%zu error=%s\n", c->png_done,c->inflate_done,(unsigned long long)c->inflate_remaining,c->next_y,c->sh,c->next_x,c->pos,held,pngle_error(png));
#endif
    pngle_destroy(png); wall_free(feed);
    return okay || wall_fail(c, "[corrupt] PNG解码未完成。");
}
static size_t wall_jpeg_input(JDEC* decoder, uint8_t* buffer, size_t count) {
    wall_context_t* c = decoder->device;
    if (!wall_poll(c)) return 0;
    if (count > c->size - c->pos) count = c->size - c->pos;
    return wall_read(c, buffer, count) ? count : 0;
}
static int wall_jpeg_output(JDEC* decoder, void* bitmap, JRECT* rect) {
    wall_context_t* c = decoder->device;
    if (!wall_poll(c)) return 0;
    unsigned left = rect->left, right = rect->right, top = rect->top, bottom = rect->bottom;
    if (left != c->jpeg_x || top != c->jpeg_y || right < left || bottom < top || right >= c->sw || bottom >= c->sh || right - left >= 16 || bottom - top >= 16)
        return wall_fail(c, "[corrupt] JPEG输出区块越界。");
    unsigned width = right - left + 1, height = bottom - top + 1;
    const uint8_t* pixels = bitmap;
    for (unsigned y = 0; y < height; ++y) memcpy(c->stripe + (size_t)y * c->sw + left, pixels + (size_t)y * width, width);
    c->jpeg_x = right + 1;
    if (c->jpeg_x == c->sw) {
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < c->sw; ++x)
                if (!wall_reduce_pixel(c, x, top + y, c->stripe[(size_t)y * c->sw + x])) return 0;
        c->jpeg_x = 0; c->jpeg_y = bottom + 1;
    }
    return 1;
}
static bool wall_decode_jpeg(wall_context_t* c) {
    if (!wall_seek(c, 0)) return false;
    void* pool = wall_alloc(WALL_JPEG_POOL);
    if (!pool) return false;
    JDEC decoder;
    JRESULT result = jd_prepare(&decoder, wall_jpeg_input, pool, WALL_JPEG_POOL, c);
    if (result != JDR_OK || decoder.width != c->source_w || decoder.height != c->source_h) {
        wall_free(pool); return wall_fail(c, "[corrupt] JPEG解码初始化失败。");
    }
    wall_fit(c, decoder.width, decoder.height);
    unsigned scale = 0;
    /* Native scaling rounds off partial edge squares. Use full scale for odd
     * dimensions so every source edge pixel contributes to the area filter. */
    while (scale < 3 && !(decoder.width % (2u << scale)) && !(decoder.height % (2u << scale)) &&
           (decoder.width >> (scale + 1)) >= c->dw * 2u && (decoder.height >> (scale + 1)) >= c->dh * 2u) ++scale;
    unsigned sw = decoder.width >> scale, sh = decoder.height >> scale;
    if (!wall_reduce_begin(c, sw, sh)) { wall_free(pool); return false; }
    c->stripe = wall_alloc((size_t)sw * 16);
    if (!c->stripe) { wall_free(pool); return false; }
    result = jd_decomp(&decoder, wall_jpeg_output, (uint8_t)scale);
    /* FASTDECODE=0 retains the current entropy byte and counts only later
     * unread bytes in dctr. No extra entropy may follow the last MCU. */
    size_t consumed = c->pos - decoder.dctr;
    bool okay = result == JDR_OK && !c->failure && c->next_y == sh && !c->next_x && !c->jpeg_x && c->jpeg_y == sh && consumed == c->jpeg_eoi;
    wall_free(pool);
    return okay || wall_fail(c, "[corrupt] JPEG数据或结束位置损坏。");
}
static bool wall_legacy_cancelled(void* context) { return !wall_poll(context); }
static bool wall_decode_legacy(wall_context_t* c, const char* path) {
    if (c->size > 2u * 1024u * 1024u || (uint64_t)c->source_w * c->source_h > 2000000u || c->source_w > 4096 || c->source_h > 4096)
        return wall_fail(c, "[format] 大图请转换为普通JPEG或PNG（不支持大型渐进/CMYK JPEG）。");
    if (!wall_poll(c)) return false;
    unsigned width = 0, height = 0;
    bool rotated = c->orientation >= 5;
    uint8_t* pixels = folio_cover_decode_legacy_jpeg_file(path, rotated ? c->image.height : c->image.width,
                                                        rotated ? c->image.width : c->image.height,
                                                        &width, &height, &c->legacy_peak, wall_legacy_cancelled, c);
    if (!pixels) return wall_poll(c) && wall_fail(c, "[legacy] 旧JPEG解码失败（图片损坏或内存不足），请转普通JPEG/PNG。");
    bool okay = wall_poll(c) && wall_reduce_begin(c, width, height);
    for (unsigned y = 0; okay && y < height; ++y)
        for (unsigned x = 0; okay && x < width; ++x) okay = wall_reduce_pixel(c, x, y, pixels[(size_t)y * width + x]);
    free(pixels);
    return okay;
}
void folio_wallpaper_image_free(folio_wallpaper_image_t* image) {
    if (!image) return;
    /* Published image allocations are detached from the active job. */
    if (image->pixels) free((wall_allocation_t*)image->pixels - 1);
    memset(image, 0, sizeof(*image));
}
/* ESP-IDF 6.1 vfs_fat_fstat() deliberately zeros times/inode/device and
 * reports only FIL size/mode. vfs_fat_stat() additionally reports FAT mtime.
 * Never compare a pathname timestamp to the descriptor's synthetic zero.
 * The test build runs the complete decoder against that exact API asymmetry. */
#if defined(ESP_PLATFORM) || defined(FOLIO_WALLPAPER_TEST_FATFS)
#define WALL_FSTAT_FULL_IDENTITY 0
#else
#define WALL_FSTAT_FULL_IDENTITY 1
#endif
static bool wall_same_path_metadata(const struct stat* a, const struct stat* b) {
    if (a->st_size != b->st_size || a->st_mode != b->st_mode || a->st_dev != b->st_dev ||
        a->st_ino != b->st_ino || a->st_mtime != b->st_mtime || a->st_ctime != b->st_ctime) return false;
#if defined(__APPLE__) && defined(st_mtime)
    if (a->st_mtimespec.tv_nsec != b->st_mtimespec.tv_nsec || a->st_ctimespec.tv_nsec != b->st_ctimespec.tv_nsec) return false;
#elif defined(__APPLE__)
    if (a->st_mtimensec != b->st_mtimensec || a->st_ctimensec != b->st_ctimensec) return false;
#elif !defined(ESP_PLATFORM)
    if (a->st_mtim.tv_nsec != b->st_mtim.tv_nsec || a->st_ctim.tv_nsec != b->st_ctim.tv_nsec) return false;
#endif
    return true;
}
static bool wall_same_fd_metadata(const struct stat* a, const struct stat* b) {
    return a->st_size == b->st_size && a->st_mode == b->st_mode &&
           (!WALL_FSTAT_FULL_IDENTITY || wall_same_path_metadata(a, b));
}
static bool wall_open_identity(const struct stat* named, const struct stat* opened) {
    return S_ISREG(named->st_mode) && S_ISREG(opened->st_mode) && named->st_size == opened->st_size &&
           (!WALL_FSTAT_FULL_IDENTITY || wall_same_path_metadata(named, opened));
}
static bool decode_wallpaper(bool fill, const char* path, unsigned target_w, unsigned target_h,
                                 folio_wallpaper_image_t* image, char* error, size_t cap) {
    if (error && cap) error[0] = 0;
    if (!image) return false;
    memset(image, 0, sizeof(*image));
    wall_context_t c = {.fill = fill, .orientation = 1, .started = wall_now()};
    c.yielded = c.started;
    bool okay = false;
    if (s_wall_context || !path || !*path || !target_w || !target_h || target_w > FOLIO_WALLPAPER_TARGET_MAX_W || target_h > FOLIO_WALLPAPER_TARGET_MAX_H) {
        if (error && cap) snprintf(error, cap, "[arguments] 壁纸参数无效或解码器忙碌。");
        return false;
    }
    s_wall_context = &c;
    if (stat(path, &c.path_original) || !S_ISREG(c.path_original.st_mode) || c.path_original.st_size <= 0) {
        wall_fail(&c, "[io] 无法读取壁纸文件。"); goto done;
    }
    c.file = fopen(path, "rb");
    if (!c.file || fstat(fileno(c.file), &c.fd_original) || !wall_open_identity(&c.path_original, &c.fd_original)) {
        wall_fail(&c, "[io] 无法读取壁纸文件或文件已变化。"); goto done;
    }
    if ((uint64_t)c.fd_original.st_size > FOLIO_WALLPAPER_INPUT_MAX) { wall_fail(&c, "[size] 壁纸不能超过32MiB。"); goto done; }
    c.size = (size_t)c.fd_original.st_size;
    (void)setvbuf(c.file, NULL, _IONBF, 0);
    c.input = wall_alloc(WALL_IO_SIZE);
    if (!c.input || !wall_poll(&c)) goto done;
    uint8_t signature[8];
    if (!wall_read(&c, signature, sizeof(signature))) goto done;
    c.png = !memcmp(signature, "\x89PNG\r\n\x1a\n", 8);
    if (c.png) { if (!wall_png_header(&c)) goto done; }
    else if (signature[0] == 255 && signature[1] == 216) { if (!wall_jpeg_header(&c)) goto done; }
    else { wall_fail(&c, "[format] 只能显示PNG或JPEG；原样上传不代表支持显示。"); goto done; }
    if (fill && target_h > target_w &&
        (c.orientation >= 5 ? c.source_h > c.source_w : c.source_w > c.source_h)) {
        static const unsigned clockwise[] = {6,6,7,8,5,2,3,4,1};
        c.orientation = clockwise[c.orientation];
    }
    c.image.width = target_w; c.image.height = target_h; c.image.stride = (target_w + 1u) / 2u;
    c.image.pixels = wall_alloc(c.image.stride * target_h);
    if (!c.image.pixels) goto done;
    memset(c.image.pixels, 255, c.image.stride * target_h);
    okay = c.png ? wall_decode_png(&c) : c.legacy ? wall_decode_legacy(&c, path) : wall_decode_jpeg(&c);
    struct stat after, named;
    if (okay && (fstat(fileno(c.file), &after) || stat(path, &named) ||
                 !wall_same_fd_metadata(&c.fd_original, &after) ||
                 !wall_same_path_metadata(&c.path_original, &named) || !wall_open_identity(&named, &after)))
        okay = wall_fail(&c, "[io] 图片在解码过程中已变化。");
    okay = okay && wall_poll(&c);
done:
    if (c.file && fclose(c.file)) okay = wall_fail(&c, "[io] 图片关闭失败。");
    wall_free(c.input); wall_free(c.horizontal); wall_free(c.accumulated); wall_free(c.stripe);
    if (okay) {
        *image = c.image;
        c.live -= ((wall_allocation_t*)c.image.pixels - 1)->size;
    } else {
        wall_free(c.image.pixels);
        if (error && cap) snprintf(error, cap, "%s", c.failure ? c.failure : "[corrupt] 图片解码失败。");
    }
    s_wall_last_peak = c.peak; s_wall_last_legacy_peak = c.legacy_peak; s_wall_last_live = c.live;
#ifdef ESP_PLATFORM
    ESP_LOGI("wallpaper", "decode %s: %ux%u -> %ux%u peak=%u legacy_extra=%u time=%llu ms", okay ? "ok" : "failed", c.source_w, c.source_h,
             target_w, target_h, (unsigned)c.peak, (unsigned)c.legacy_peak, (unsigned long long)(wall_now() - c.started));
#endif
    s_wall_context = NULL;
    return okay;
}
bool folio_wallpaper_decode_file(const char* path, unsigned target_w, unsigned target_h,
                                 folio_wallpaper_image_t* image, char* error, size_t cap) {
    return decode_wallpaper(false, path, target_w, target_h, image, error, cap);
}
bool folio_wallpaper_decode_cropped(const char* path, unsigned target_w, unsigned target_h,
                                 folio_wallpaper_image_t* image, char* error, size_t cap) {
    return decode_wallpaper(true, path, target_w, target_h, image, error, cap);
}
bool folio_wallpaper_draw_image(const folio_wallpaper_image_t* image, uint8_t* framebuffer, EpdRect rect) {
    if (!image || !image->pixels || !image->width || !image->height || image->width > FOLIO_WALLPAPER_TARGET_MAX_W || image->height > FOLIO_WALLPAPER_TARGET_MAX_H ||
        image->stride != (image->width + 1u) / 2u || !framebuffer || rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        (int64_t)rect.x + rect.width > epd_rotated_display_width() || (int64_t)rect.y + rect.height > epd_rotated_display_height() || folio_io_cancelled()) return false;
    unsigned w = (unsigned)rect.width, h = (unsigned)((uint64_t)image->height * w / image->width);
    if (h > (unsigned)rect.height) { h = (unsigned)rect.height; w = (unsigned)((uint64_t)image->width * h / image->height); }
    if (!w) w = 1;
    if (!h) h = 1;
    int left = rect.x + (rect.width - (int)w) / 2, top = rect.y + (rect.height - (int)h) / 2;
    /* Commit has no cancellation points: a successful validation paints the
     * entire rect, never a half old/half new image. No I/O or allocations here. */
    for (int y = rect.y; y < rect.y + rect.height; ++y) for (int x = rect.x; x < rect.x + rect.width; ++x) {
        unsigned level = 15;
        if (x >= left && x < left + (int)w && y >= top && y < top + (int)h) {
            unsigned sx = (unsigned)((uint64_t)(x - left) * image->width / w), sy = (unsigned)((uint64_t)(y - top) * image->height / h);
            uint8_t packed = image->pixels[(size_t)sy * image->stride + sx / 2];
            level = (sx & 1) ? packed & 15 : packed >> 4;
        }
        epd_draw_pixel(x, y, (uint8_t)(level * 17), framebuffer);
    }
    return true;
}
