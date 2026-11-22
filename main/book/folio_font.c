#include "folio_font.h"
#include "folio_font_pack.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "epdiy.h"
#include "folio_text.h"
#if defined(ESP_PLATFORM) || defined(FOLIO_TEST_CARD_FONT)
#include "psa/crypto.h"
#endif
#ifdef ESP_PLATFORM
#include "miniz.h"
#include "esp_partition.h"
#include "../assets/folio_builtin.h"
#elif defined(FOLIO_FONT_PACK_PATH)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>
#endif
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static void* font_raster_alloc(size_t size, void* context) {
    // Scratch bitmaps must not make an otherwise usable face disappear when
    // PSRAM is fragmented or occupied by a book. Small allocations may use IRAM.
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(size);
    if (!p && context) *(bool*)context = true;
    return p;
}
#define STBTT_malloc(size, context) font_raster_alloc((size), (context))
#define STBTT_free(pointer, context) ((void)(context), free(pointer))
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "vendor/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define FOLIO_GLYPH_CACHE_N 96
#define SHARED_GLYPH_CACHE_N 1024
#define FOLIO_FONT_MAX_EXPANDED_POINTS 8192
#define FOLIO_FONT_MAX_COMPONENT_DEPTH 16
#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif
#define FOLIO_CARD_FONT_DIR FOLIO_SD_ROOT "/fonts"

static void font_yield(void) {
#ifdef ESP_PLATFORM
    vTaskDelay(1);
#endif
}

typedef struct {
    uint32_t codepoint;
    uint32_t age;
    int pixel_height;
    int width;
    int height;
    int xoff;
    int yoff;
    uint16_t ink_left, ink_top, ink_right, ink_bottom;
    int advance;
    int left_side_bearing;
    uint8_t* pixels;
} glyph_cache_entry_t;

// Large card faces keep only metrics and a validated, remapped glyph tree in RAM.
#define CARD_RESIDENT_LIMIT (512u * 1024u)
#define CARD_TREE_LIMIT (256u * 1024u)
#define CARD_TREE_N 256u
#define CARD_LOCA_BYTES ((CARD_TREE_N + 1) * 4u)
#define CACHE_BITMAP_LIMIT (768u * 1024u)

typedef struct {
    FILE* file;
    uint32_t glyf_offset, glyf_length;
    uint32_t var_offsets, var_data, var_length;
    uint16_t tuple_count, tuple_peaks[64*8];
    unsigned axis_count, weight_axis;
    float coords[5];
    bool wide_offsets;
    uint8_t* tree;
    size_t capacity;
    uint16_t ids[CARD_TREE_N];
    uint8_t states[CARD_TREE_N], depths[CARD_TREE_N];
    uint16_t radii[CARD_TREE_N];
    uint32_t costs[CARD_TREE_N];
} card_face_t;

struct folio_font {
    card_face_t* card;
    int weight_step;
    size_t cache_bytes;
    uint8_t* bytes;
    size_t length;
    stbtt_fontinfo info;
    float scale;
    float visual_height, visual_top;
    int pixel_height;
    int ascent;
    int line_height;
    uint16_t cmap_segments;
    uint32_t tick;
    glyph_cache_entry_t local_cache[FOLIO_GLYPH_CACHE_N];
    glyph_cache_entry_t *cache;
    unsigned cache_slots;
    uint32_t* advances; // ADVANCE_CACHE_N entries: cp<<8 | pixel_height, advance in the low half.
    struct folio_font* fallback;
};
#define ADVANCE_CACHE_N 4096u

static const struct { const char* path; const char* label; const char* resource; } builtin_fonts[] = {
    {FOLIO_FONT_BUILTIN_PATH, "内置宋体", NULL},
    {":builtin:hei", "Folio 黑体", "hei"},
    {":builtin:kai", "Folio 楷体", "kai"},
    {":builtin:yuan", "Folio 圆体", "yuan"},
};
_Static_assert(sizeof(builtin_fonts)/sizeof(builtin_fonts[0]) == FOLIO_FONT_BUILTIN_COUNT, "builtin registry size");

bool folio_font_is_builtin_path(const char* path) {
    return path && !strncmp(path, FOLIO_FONT_BUILTIN_PATH, sizeof(FOLIO_FONT_BUILTIN_PATH)-1);
}
const char* folio_font_builtin_path(unsigned index) {
    return index < FOLIO_FONT_BUILTIN_COUNT ? builtin_fonts[index].path : NULL;
}
const char* folio_font_builtin_label(const char* path) {
    if(path) for(unsigned i=0;i<FOLIO_FONT_BUILTIN_COUNT;++i)
        if(!strcmp(path,builtin_fonts[i].path))return builtin_fonts[i].label;
    return NULL;
}

typedef struct {
    const uint8_t* data;
    size_t length;
    uint32_t offset;
} table_view_t;

static uint16_t be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool has_tag(const uint8_t* tag, const char* text) {
    return memcmp(tag, text, 4) == 0;
}

static bool contains_range(size_t length, uint32_t offset, uint32_t count) {
    return offset <= length && count <= length - offset;
}

// The renderer only consumes glyph indices. Do not expose unvalidated cmap search
// accelerators or optional kern/GPOS tables to stb_truetype.
static int font_glyph_index(const folio_font_t* font, uint32_t cp) {
    if (cp > 0xFFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    const uint8_t* sub = font->bytes + font->info.index_map;
    uint32_t n = font->cmap_segments;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (be16(sub + 14 + mid * 2) < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo == n) return 0;
    uint32_t starts = 16 + n * 2, deltas = starts + n * 2, ranges = deltas + n * 2;
    uint16_t start = be16(sub + starts + lo * 2);
    if (cp < start) return 0;
    uint16_t delta = be16(sub + deltas + lo * 2), range = be16(sub + ranges + lo * 2);
    uint16_t glyph = range ? be16(sub + ranges + lo * 2 + range + (cp - start) * 2) : (uint16_t)cp;
    // The delta applies to nonzero glyphIdArray values as well (OpenType cmap4).
    if (!range || glyph != 0) glyph = (uint16_t)(glyph + delta);
    return glyph < font->info.numGlyphs ? glyph : 0;
}

static folio_font_t* glyph_source(folio_font_t* font, uint32_t cp) {
    if(font->fallback && !font_glyph_index(font,cp) && font_glyph_index(font->fallback,cp))return font->fallback;
    return font;
}

static bool read_loca(const table_view_t* loca, bool long_format, uint16_t index, uint32_t* out) {
    uint32_t offset = long_format ? (uint32_t)index * 4 : (uint32_t)index * 2;
    uint32_t bytes = long_format ? 4 : 2;
    if (!contains_range(loca->length, offset, bytes)) return false;
    *out = long_format ? be32(loca->data + offset) : (uint32_t)be16(loca->data + offset) * 2;
    return true;
}

static bool validate_simple_glyph(const uint8_t* glyph, size_t length, uint16_t contours) {
    if (length < 10 || contours > 512) return false;
    if (contours == 0) return true;

    uint32_t pos = 10;
    uint32_t endpoints_bytes = (uint32_t)contours * 2;
    if (!contains_range(length, pos, endpoints_bytes + 2)) return false;
    uint16_t previous = 0;
    for (uint16_t i = 0; i < contours; ++i) {
        uint16_t endpoint = be16(glyph + pos + (uint32_t)i * 2);
        if (i && endpoint <= previous) return false;
        previous = endpoint;
    }
    uint32_t points = (uint32_t)previous + 1;
    if (points > 8192) return false;
    pos += endpoints_bytes;
    uint16_t instructions = be16(glyph + pos);
    pos += 2;
    if (instructions > 8192 || !contains_range(length, pos, instructions)) return false;
    pos += instructions;

    uint8_t* flags = malloc(points);
    if (flags == NULL) return false;
    uint32_t expanded = 0;
    while (expanded < points) {
        if (!contains_range(length, pos, 1)) { free(flags); return false; }
        uint8_t flag = glyph[pos++];
        uint32_t repeat = 1;
        if (flag & 0x08) {
            if (!contains_range(length, pos, 1)) { free(flags); return false; }
            repeat += glyph[pos++];
        }
        if (repeat > points - expanded) { free(flags); return false; }
        memset(flags + expanded, flag, repeat);
        expanded += repeat;
    }

    // stb's first off-curve point accesses the following point. A one-point
    // contour can therefore only begin on the curve.
    uint32_t contour_start = 0;
    for (uint16_t i = 0; i < contours; ++i) {
        uint32_t contour_end = be16(glyph + 10 + (uint32_t)i * 2);
        if (contour_start == contour_end && !(flags[contour_start] & 1)) {
            free(flags);
            return false;
        }
        contour_start = contour_end + 1;
    }

    uint32_t x_bytes = 0, y_bytes = 0;
    for (uint32_t i = 0; i < points; ++i) {
        x_bytes += (flags[i] & 0x02) ? 1 : ((flags[i] & 0x10) ? 0 : 2);
        y_bytes += (flags[i] & 0x04) ? 1 : ((flags[i] & 0x20) ? 0 : 2);
    }
    if (!contains_range(length, pos, x_bytes + y_bytes)) { free(flags); return false; }
    // Bounds from glyf are used for bitmap allocation, so all control points
    // must stay inside them, and the int16 geometry must not wrap.
    int16_t limits[4] = {(int16_t)be16(glyph + 2), (int16_t)be16(glyph + 4),
                         (int16_t)be16(glyph + 6), (int16_t)be16(glyph + 8)};
    for (unsigned axis = 0; axis < 2; ++axis) {
        int32_t value = 0;
        uint8_t short_bit = axis ? 0x04 : 0x02, same_bit = axis ? 0x20 : 0x10;
        for (uint32_t i = 0; i < points; ++i) {
            if (flags[i] & short_bit) {
                int delta = glyph[pos++];
                value += (flags[i] & same_bit) ? delta : -delta;
            } else if (!(flags[i] & same_bit)) {
                value += (int16_t)be16(glyph + pos);
                pos += 2;
            }
            if (value < limits[axis] || value > limits[axis + 2] ||
                value < -32767 || value > 32767) { free(flags); return false; }
        }
    }
    free(flags);
    return true;
}

static bool validate_composite_glyph(const uint8_t* glyph, size_t length, uint16_t glyph_count) {
    if (length < 12) return false;
    uint32_t pos = 10;
    uint16_t flags;
    unsigned components = 0;
    do {
        if (!contains_range(length, pos, 4)) return false;
        flags = be16(glyph + pos);
        uint16_t component = be16(glyph + pos + 2);
        if (component >= glyph_count) return false;
        uint16_t transform = flags & (0x0008 | 0x0040 | 0x0080);
        if ((transform & (transform - 1)) != 0 || !(flags & 0x0002)) return false;
        pos += 4;
        uint32_t args_bytes = (flags & 0x0001) ? 4 : 2;
        uint32_t transform_bytes = (flags & 0x0008) ? 2 : ((flags & 0x0040) ? 4 : ((flags & 0x0080) ? 8 : 0));
        if (!contains_range(length, pos, args_bytes + transform_bytes)) return false;
        pos += args_bytes + transform_bytes;
        if (++components > 64) return false;
    } while (flags & 0x0020);

    if (flags & 0x0100) {
        if (!contains_range(length, pos, 2)) return false;
        uint16_t instructions = be16(glyph + pos);
        pos += 2;
        if (instructions > 8192 || !contains_range(length, pos, instructions)) return false;
    }
    return true;
}

static bool validate_cmap4(const table_view_t* cmap, uint16_t glyph_count,
                           uint32_t* selected_offset) {
    if (cmap->length < 4 || be16(cmap->data) != 0) return false;
    uint16_t records = be16(cmap->data + 2);
    if (records == 0 || records > 32 || !contains_range(cmap->length, 4, (uint32_t)records * 8)) return false;

    bool found = false;
    uint32_t records_end = 4 + (uint32_t)records * 8;
    for (uint16_t record = 0; record < records; ++record) {
        uint32_t record_pos = 4 + (uint32_t)record * 8;
        uint16_t platform = be16(cmap->data + record_pos);
        uint16_t encoding = be16(cmap->data + record_pos + 2);
        if (platform != 0 && !(platform == 3 && (encoding == 1 || encoding == 10))) continue;
        uint32_t sub_offset = be32(cmap->data + record_pos + 4);
        if (!contains_range(cmap->length, sub_offset, 2) || be16(cmap->data + sub_offset) != 4) continue;
        if (sub_offset < records_end) return false;
        if (!contains_range(cmap->length, sub_offset, 8)) return false;
        uint16_t sub_length = be16(cmap->data + sub_offset + 2);
        if (sub_length < 16 || !contains_range(cmap->length, sub_offset, sub_length)) return false;
        const uint8_t* sub = cmap->data + sub_offset;
        uint16_t seg_count_x2 = be16(sub + 6);
        if (seg_count_x2 == 0 || (seg_count_x2 & 1)) return false;
        uint32_t seg_count = seg_count_x2 / 2;
        if (seg_count > 4096) return false;
        unsigned selector = 0;
        while ((2u << selector) <= seg_count) ++selector;
        uint16_t search_range = (uint16_t)(2u << selector);
        if (be16(sub + 8) != search_range || be16(sub + 10) != selector ||
            be16(sub + 12) != seg_count_x2 - search_range) return false;
        uint32_t ends = 14;
        uint32_t starts = ends + 2 * seg_count + 2;
        uint32_t deltas = starts + 2 * seg_count;
        uint32_t ranges = deltas + 2 * seg_count;
        if (!contains_range(sub_length, ends, 2 * seg_count) ||
            !contains_range(sub_length, starts, 2 * seg_count) ||
            !contains_range(sub_length, deltas, 2 * seg_count) ||
            !contains_range(sub_length, ranges, 2 * seg_count)) return false;
        uint16_t previous_end = 0;
        for (uint32_t i = 0; i < seg_count; ++i) {
            uint16_t end = be16(sub + ends + i * 2);
            uint16_t start = be16(sub + starts + i * 2);
            uint16_t range = be16(sub + ranges + i * 2);
            uint16_t delta = be16(sub + deltas + i * 2);
            if (start > end || (i > 0 && start <= previous_end) || (range & 1)) return false;
            previous_end = end;
            if (range != 0) {
                uint32_t range_entry = ranges + i * 2;
                uint32_t glyph_pos = range_entry + range;
                uint32_t glyph_bytes = ((uint32_t)end - start + 1) * 2;
                if (glyph_pos < ranges + seg_count * 2 ||
                    !contains_range(sub_length, glyph_pos, glyph_bytes)) return false;
            }
            for (uint32_t cp = start; cp <= end; ++cp) {
                uint16_t gid = range ? be16(sub + ranges + i * 2 + range + (cp - start) * 2) : (uint16_t)cp;
                if (!range || gid != 0) gid = (uint16_t)(gid + delta);
                if (gid >= glyph_count) return false;
            }
        }
        if (previous_end != 0xFFFF) return false;
        if (selected_offset != NULL) *selected_offset = cmap->offset + sub_offset;
        found = true;
    }
    return found;
}

static bool validate_component_tree(const table_view_t* loca, const table_view_t* glyf,
                                    bool long_loca, uint16_t glyph_count, uint16_t glyph_index,
                                    uint8_t* state, uint32_t* expanded_points,
                                    uint8_t* depth_cache, uint16_t* radius_cache, unsigned depth) {
    if (depth > FOLIO_FONT_MAX_COMPONENT_DEPTH || glyph_index >= glyph_count) return false;
    if (state[glyph_index] == 1) return false;
    if (state[glyph_index] == 2) {
        return expanded_points[glyph_index] <= FOLIO_FONT_MAX_EXPANDED_POINTS &&
               (unsigned)depth + depth_cache[glyph_index] - 1 <= FOLIO_FONT_MAX_COMPONENT_DEPTH;
    }
    state[glyph_index] = 1;

    uint32_t start, end;
    if (!read_loca(loca, long_loca, glyph_index, &start) ||
        !read_loca(loca, long_loca, (uint16_t)(glyph_index + 1), &end) ||
        end < start || end > glyf->length) return false;
    uint32_t expanded = 1; // Count even empty components to bound exponential trees.
    uint8_t subtree_depth = 1;
    uint16_t radius = 0;
    if (start != end) {
        const uint8_t* glyph = glyf->data + start;
        if (end - start < 10) return false;
        int16_t contours = (int16_t)be16(glyph);
        if (contours < 0) {
            uint32_t pos = 10;
            uint16_t flags;
            do {
                if (!contains_range(end - start, pos, 4)) return false;
                flags = be16(glyph + pos);
                uint16_t component = be16(glyph + pos + 2);
                uint32_t child_points;
                if (!validate_component_tree(loca, glyf, long_loca, glyph_count, component,
                                             state, expanded_points, depth_cache, radius_cache, depth + 1)) return false;
                child_points = expanded_points[component];
                if (child_points > FOLIO_FONT_MAX_EXPANDED_POINTS - expanded) return false;
                expanded += child_points;
                if (depth_cache[component] >= subtree_depth) {
                    subtree_depth = depth_cache[component] + 1;
                }
                pos += 4;
                uint32_t args_bytes = (flags & 0x0001) ? 4 : 2;
                uint32_t transform_bytes = (flags & 0x0008) ? 2 :
                                           ((flags & 0x0040) ? 4 : ((flags & 0x0080) ? 8 : 0));
                // stb applies the matrix and then its column norms to int16
                // vertices. Bound that exact computation at every tree level.
                float tx = (flags & 1) ? (int16_t)be16(glyph + pos) : (int8_t)glyph[pos];
                float ty = (flags & 1) ? (int16_t)be16(glyph + pos + 2) : (int8_t)glyph[pos + 1];
                uint32_t matrix_pos = pos + args_bytes;
                float a = 1, b = 0, c = 0, d = 1;
                if (flags & 0x0008) a = d = (int16_t)be16(glyph + matrix_pos) / 16384.0f;
                else if (flags & 0x0040) {
                    a = (int16_t)be16(glyph + matrix_pos) / 16384.0f;
                    d = (int16_t)be16(glyph + matrix_pos + 2) / 16384.0f;
                } else if (flags & 0x0080) {
                    a = (int16_t)be16(glyph + matrix_pos) / 16384.0f;
                    b = (int16_t)be16(glyph + matrix_pos + 2) / 16384.0f;
                    c = (int16_t)be16(glyph + matrix_pos + 4) / 16384.0f;
                    d = (int16_t)be16(glyph + matrix_pos + 6) / 16384.0f;
                }
                float child_radius = radius_cache[component];
                float bound_x = sqrtf(a * a + b * b) * ((fabsf(a) + fabsf(c)) * child_radius + fabsf(tx));
                float bound_y = sqrtf(c * c + d * d) * ((fabsf(b) + fabsf(d)) * child_radius + fabsf(ty));
                float bound = fmaxf(bound_x, bound_y);
                if (!isfinite(bound) || bound > 32767.0f) return false;
                uint16_t component_radius = (uint16_t)ceilf(bound);
                if (component_radius > radius) radius = component_radius;
                pos += args_bytes + transform_bytes;
            } while (flags & 0x0020);
        } else if (contours > 0) {
            expanded += (uint32_t)be16(glyph + 10 + (uint32_t)(contours - 1) * 2) + 1 + 2u * contours;
            if (expanded > FOLIO_FONT_MAX_EXPANDED_POINTS) return false;
            for (unsigned axis = 0; axis < 4; ++axis) {
                int coordinate = (int16_t)be16(glyph + 2 + axis * 2);
                int magnitude = coordinate < 0 ? -coordinate : coordinate;
                if (magnitude > 32767) return false;
                if (magnitude > radius) radius = (uint16_t)magnitude;
            }
        }
    }
    if ((unsigned)depth + subtree_depth - 1 > FOLIO_FONT_MAX_COMPONENT_DEPTH) return false;
    expanded_points[glyph_index] = expanded;
    depth_cache[glyph_index] = subtree_depth;
    radius_cache[glyph_index] = radius;
    state[glyph_index] = 2;
    return true;
}

__attribute__((weak,noinline)) bool folio_io_cancelled(void) { return false; }
static bool font_cancelled(void) { return folio_io_cancelled(); }

static bool validate_metrics(table_view_t head, table_view_t maxp, table_view_t hhea,
                             table_view_t hmtx, table_view_t loca, table_view_t glyf,
                             table_view_t cmap, uint32_t* cmap_out) {
    if (head.length < 54 || maxp.length < 6 || hhea.length < 36 || hmtx.length < 4 ||
        loca.length < 4 || glyf.length < 10) return false;

    uint16_t glyph_count = be16(maxp.data + 4);
    uint16_t metric_count = be16(hhea.data + 34);
    int16_t loca_format = (int16_t)be16(head.data + 50);
    if (glyph_count == 0 || metric_count == 0 || metric_count > glyph_count ||
        (loca_format != 0 && loca_format != 1)) return false;
    int ascent = (int16_t)be16(hhea.data + 4), descent = (int16_t)be16(hhea.data + 6);
    int line_gap = (int16_t)be16(hhea.data + 8);
    int em_height = ascent - descent;
    if (be16(head.data + 18) < 16 || be16(head.data + 18) > 16384 ||
        ascent <= 0 || descent > 0 || em_height < 16 || em_height > 65534 ||
        line_gap < 0 || line_gap > em_height) return false;
    uint32_t cmap4_offset = 0;
    if (!validate_cmap4(&cmap, glyph_count, &cmap4_offset)) return false;
    uint32_t required_metrics = (uint32_t)metric_count * 4 + (uint32_t)(glyph_count - metric_count) * 2;
    if (hmtx.length < required_metrics) return false;
    for (uint16_t i = 0; i < metric_count; ++i) {
        if (be16(hmtx.data + (uint32_t)i * 4) > (uint32_t)em_height * 4) return false;
    }
    uint32_t need_loca = (uint32_t)(glyph_count + 1) * (loca_format ? 4 : 2);
    if (loca.length < need_loca) return false;

    *cmap_out = cmap4_offset;
    return true;
}

static bool validate_ttf(const uint8_t* data, size_t length, uint32_t* cmap4_offset_out) {
    if (font_cancelled()) return false;
    if (data == NULL || length < 12 || length > FOLIO_FONT_MAX_BYTES) return false;
    uint32_t signature = be32(data);
    if (signature != 0x00010000 && signature != 0x74727565) return false; // 1.0 or "true"
    uint16_t table_count = be16(data + 4);
    if (table_count < 6 || table_count > 64 || !contains_range(length, 12, (uint32_t)table_count * 16)) return false;

    table_view_t head = {0}, maxp = {0}, hhea = {0}, hmtx = {0}, loca = {0}, glyf = {0}, cmap = {0};
    for (uint16_t i = 0; i < table_count; ++i) {
        const uint8_t* entry = data + 12 + (uint32_t)i * 16;
        for (uint16_t previous = 0; previous < i; ++previous) {
            if (memcmp(entry, data + 12 + (uint32_t)previous * 16, 4) == 0) return false;
        }
        uint32_t offset = be32(entry + 8), size = be32(entry + 12);
        if (!contains_range(length, offset, size) || (size && offset < 12u + (uint32_t)table_count * 16u)) return false;
        table_view_t table = {data + offset, size, offset};
        if (has_tag(entry, "head")) head = table;
        else if (has_tag(entry, "maxp")) maxp = table;
        else if (has_tag(entry, "hhea")) hhea = table;
        else if (has_tag(entry, "hmtx")) hmtx = table;
        else if (has_tag(entry, "loca")) loca = table;
        else if (has_tag(entry, "glyf")) glyf = table;
        else if (has_tag(entry, "cmap")) cmap = table;
    }
    uint32_t cmap4_offset;
    if (!validate_metrics(head, maxp, hhea, hmtx, loca, glyf, cmap, &cmap4_offset)) return false;
    uint16_t glyph_count = be16(maxp.data + 4);
    int16_t loca_format = (int16_t)be16(head.data + 50);
    uint32_t previous = 0;
    for (uint16_t i = 0; i < glyph_count; ++i) {
        if ((i & 127u) == 0) { if (font_cancelled()) return false; font_yield(); }
        uint32_t start, end;
        if (!read_loca(&loca, loca_format != 0, i, &start) ||
            !read_loca(&loca, loca_format != 0, (uint16_t)(i + 1), &end) ||
            start < previous || end < start || end > glyf.length) return false;
        previous = end;
        if (start == end) continue;
        const uint8_t* glyph = glyf.data + start;
        size_t glyph_length = end - start;
        if (glyph_length < 10) return false;
        if ((int16_t)be16(glyph + 2) > (int16_t)be16(glyph + 6) ||
            (int16_t)be16(glyph + 4) > (int16_t)be16(glyph + 8)) return false;
        int16_t contours = (int16_t)be16(glyph);
        if (contours >= 0) {
            if (!validate_simple_glyph(glyph, glyph_length, (uint16_t)contours)) return false;
        } else if (!validate_composite_glyph(glyph, glyph_length, glyph_count)) {
            return false;
        }
    }

    uint8_t* state = calloc(glyph_count, sizeof(*state));
    uint32_t* expanded_points = calloc(glyph_count, sizeof(*expanded_points));
    uint8_t* depth_cache = calloc(glyph_count, sizeof(*depth_cache));
    uint16_t* radius_cache = calloc(glyph_count, sizeof(*radius_cache));
    if (state == NULL || expanded_points == NULL || depth_cache == NULL || radius_cache == NULL) {
        free(state);
        free(expanded_points);
        free(depth_cache);
        free(radius_cache);
        return false;
    }
    bool valid_components = true;
    for (uint16_t i = 0; i < glyph_count; ++i) {
        if ((i & 127u) == 0) { if (font_cancelled()) { valid_components=false; break; } font_yield(); }
        if (!validate_component_tree(&loca, &glyf, loca_format != 0, glyph_count, i,
                                     state, expanded_points, depth_cache, radius_cache, 0)) {
            valid_components = false;
            break;
        }
    }
    free(state);
    free(expanded_points);
    free(depth_cache);
    free(radius_cache);
    if (!valid_components) return false;
    if (cmap4_offset_out != NULL) *cmap4_offset_out = cmap4_offset;
    return true;
}

static void* font_alloc(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) ptr = malloc(size);
    return ptr;
}

static bool card_read(FILE* file, uint32_t offset, void* dst, size_t size) {
    return !font_cancelled() && (ftell(file) == (long)offset || fseek(file, (long)offset, SEEK_SET) == 0) &&
           fread(dst, 1, size, file) == size;
}

static void store_be32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}

#include "folio_gvar.inc"

static bool grow_glyph_tree(card_face_t* card,size_t used,size_t size) {
    if (size>CARD_TREE_LIMIT-used) return false;
    if (used+size<=card->capacity) return true;
    size_t cap=card->capacity?card->capacity:4096;
    while(cap<used+size) cap*=2;
    uint8_t* tree=font_alloc(cap);
    if (!tree) return false;
    if (card->tree) memcpy(tree,card->tree,used);
    free(card->tree);card->tree=tree;card->capacity=cap;
    return true;
}
// Construct a private glyph snapshot. Never pass mutable SD data to stb before
// validating the exact bytes it will render (including referenced components).
static bool card_glyph(folio_font_t* font, uint16_t gid, stbtt_fontinfo* info) {
    card_face_t* card = font->card;
    size_t used = CARD_LOCA_BYTES;
    unsigned count = 1;
    card->ids[0] = gid;
    for (unsigned i = 0; i < count; ++i) {
        unsigned id = card->ids[i];
        const uint8_t* loc = font->bytes + font->info.loca;
        bool wide = font->info.indexToLocFormat != 0;
        uint32_t start = wide ? be32(loc + id*4) : be16(loc + id*2)*2u;
        uint32_t end = wide ? be32(loc + id*4 + 4) : be16(loc + id*2 + 2)*2u;
        if (end < start || end > card->glyf_length || end-start > CARD_TREE_LIMIT-used) return false;
        size_t size = end-start;
        if (!grow_glyph_tree(card,used,size)) return false;
        store_be32(card->tree + i*4, used-CARD_LOCA_BYTES);
        uint8_t* glyph = card->tree + used;
        used += size;
        if (!size) continue;
        if (size < 10 || !card_read(card->file, card->glyf_offset + start, glyph, size) ||
            (int16_t)be16(glyph+2) > (int16_t)be16(glyph+6) ||
            (int16_t)be16(glyph+4) > (int16_t)be16(glyph+8)) return false;
        int16_t contours = (int16_t)be16(glyph);
        if (contours >= 0) {
            if (!validate_simple_glyph(glyph, size, contours)) return false;
            if (font->weight_step && card->var_data) {
                uint8_t* varied=NULL;
                size_t new_size=0;
                if (!var_simple(card,id,font->weight_step,glyph,contours,&varied,&new_size)) return false;
                if (varied) {
                    used-=size;
                    bool ok=grow_glyph_tree(card,used,new_size) && validate_simple_glyph(varied,new_size,contours);
                    if (ok) {memcpy(card->tree+used,varied,new_size);used+=new_size;}
                    free(varied);
                    if (!ok) return false;
                }
            }
        } else {
            if (!validate_composite_glyph(glyph, size, font->info.numGlyphs)) return false;
            unsigned pos = 10, flags;
            do {
                flags = be16(glyph + pos);
                unsigned child = be16(glyph + pos + 2), j = 0;
                while (j < count && card->ids[j] != child) ++j;
                if (j == count) {
                    if (count == CARD_TREE_N) return false;
                    card->ids[count++] = child;
                }
                glyph[pos+2] = j >> 8; glyph[pos+3] = j;
                pos += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 64) ? 4 : (flags & 128) ? 8 : 0);
            } while (flags & 32);
        }
    }
    store_be32(card->tree + count*4, used-CARD_LOCA_BYTES);
    table_view_t loca = {card->tree, (count+1)*4, 0};
    table_view_t glyf = {card->tree+CARD_LOCA_BYTES, used-CARD_LOCA_BYTES, 0};
    memset(card->states, 0, count);
    if (!validate_component_tree(&loca, &glyf, true, count, 0, card->states, card->costs,
                                 card->depths, card->radii, 0)) return false;
    *info = (stbtt_fontinfo){.data=card->tree, .loca=0, .glyf=CARD_LOCA_BYTES,
                             .indexToLocFormat=1, .numGlyphs=(int)count};
    return true;
}

static void evict_bitmap(folio_font_t* font, glyph_cache_entry_t* entry) {
    if (entry->pixels && entry->pixels != (uint8_t*)font) {
        font->cache_bytes -= (size_t)entry->width * entry->height;
        free(entry->pixels);
    }
    memset(entry, 0, sizeof(*entry));
}

bool folio_font_has_weight(const folio_font_t* font) {
    return font && font->card && font->card->var_data;
}
void folio_font_set_weight(folio_font_t* font,int step) {
    if (!folio_font_has_weight(font) || step < -2 || step > 2 || font->weight_step==step) return;
    font->weight_step=step;
    for (unsigned i=0;i<font->cache_slots;++i) evict_bitmap(font,&font->cache[i]);
}

void folio_font_flush_cache(folio_font_t* font) {
    if (font) for (unsigned i = 0; i < font->cache_slots; ++i) evict_bitmap(font, &font->cache[i]);
}

static glyph_cache_entry_t* glyph_get(folio_font_t* font, uint32_t cp) {
    for (unsigned i = 0; i < font->cache_slots; ++i) {
        if (font->cache[i].pixels != NULL && font->cache[i].codepoint == cp &&
            font->cache[i].pixel_height == font->pixel_height) {
            font->cache[i].age = ++font->tick;
            return &font->cache[i];
        }
    }

    folio_font_t* source = glyph_source(font,cp);
    int glyph_index = font_glyph_index(source, cp);
    if (glyph_index <= 0) return NULL;
    unsigned slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (unsigned i = 0; i < font->cache_slots; ++i) {
        if (font->cache[i].pixels == NULL) { slot = i; oldest = 0; break; }
        if (font->cache[i].age < oldest) { oldest = font->cache[i].age; slot = i; }
    }

    glyph_cache_entry_t* entry = &font->cache[slot];
    stbtt_fontinfo snapshot;
    stbtt_fontinfo* info = &source->info;
    if (source->card) {
        if (!card_glyph(source, glyph_index, &snapshot)) return NULL;
        info = &snapshot; glyph_index = 0;
    }
    evict_bitmap(font, entry);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(info, glyph_index, source->scale, source->scale, &x0, &y0, &x1, &y1);
    int width = x1 - x0, height = y1 - y0;
    if (width < 0 || height < 0 || width > font->pixel_height * 4 || height > font->pixel_height * 4) return NULL;
    size_t bytes = (size_t)width * (size_t)height;
    if (bytes > CACHE_BITMAP_LIMIT) return NULL;
    while (bytes > CACHE_BITMAP_LIMIT - font->cache_bytes) {
        glyph_cache_entry_t* victim = NULL;
        for (unsigned i = 0; i < font->cache_slots; ++i) {
            glyph_cache_entry_t* p = &font->cache[i];
            if (p->pixels && p->pixels != (uint8_t*)font && (!victim || p->age < victim->age)) victim = p;
        }
        if (!victim) return NULL;
        evict_bitmap(font, victim);
    }
    uint8_t* pixels = bytes ? font_alloc(bytes) : NULL;
    if (bytes && pixels == NULL) return NULL;
    if (bytes) {
        memset(pixels, 0, bytes);
        bool oom = false;
        stbtt_fontinfo raster = *info;
        raster.userdata = &oom;
        stbtt_MakeGlyphBitmap(&raster, pixels, width, height, width,
                             source->scale, source->scale, glyph_index);
        if (oom) { free(pixels); return NULL; }
        font->cache_bytes += bytes;
        for(size_t i=0;i<bytes;++i) {
            unsigned a=pixels[i];pixels[i]=(uint8_t)(a+a*(255-a)/510);
        }
    }
    entry->codepoint = cp;
    entry->pixel_height = font->pixel_height;
    entry->age = ++font->tick;
    entry->width = width;
    entry->height = height;
    entry->xoff = x0;
    // Align the fallback's calibrated text top with the primary face's text top.
    entry->yoff = y0 + source->ascent - font->ascent;
    // A black/white pixel survives the 16-level blend iff alpha >= 9.
    // Store offsets inside the bitmap (bounded to 4 * 144 pixels), not another
    // bitmap; callers can subsequently measure labels without scanning pixels.
    unsigned left = (unsigned)width, top = (unsigned)height, right = 0, bottom = 0;
    for (int row = 0; row < height; ++row) for (int col = 0; col < width; ++col) {
        if (pixels[(size_t)row * width + col] < 9) continue;
        if ((unsigned)col < left) left = (unsigned)col;
        if ((unsigned)row < top) top = (unsigned)row;
        if ((unsigned)col + 1 > right) right = (unsigned)col + 1;
        if ((unsigned)row + 1 > bottom) bottom = (unsigned)row + 1;
    }
    if (right && bottom) {
        entry->ink_left = (uint16_t)left;
        entry->ink_top = (uint16_t)top;
        entry->ink_right = (uint16_t)right;
        entry->ink_bottom = (uint16_t)bottom;
    }
    entry->pixels = pixels ? pixels : (uint8_t*)font;
    return entry;
}

static folio_font_t* font_alloc_state(void) {
    folio_font_t* font = font_alloc(sizeof(*font));
    if (font != NULL) {
        memset(font, 0, sizeof(*font));
        // A CJK page holds hundreds of distinct glyphs; a small LRU misses on
        // nearly every character and re-rasterises the whole page each turn.
        glyph_cache_entry_t* cache = font_alloc(SHARED_GLYPH_CACHE_N * sizeof(*cache));
        if (cache) { memset(cache, 0, SHARED_GLYPH_CACHE_N * sizeof(*cache)); font->cache = cache; font->cache_slots = SHARED_GLYPH_CACHE_N; }
        else { font->cache = font->local_cache; font->cache_slots = FOLIO_GLYPH_CACHE_N; }
    }
    return font;
}

#if defined(ESP_PLATFORM) || defined(FOLIO_FONT_PACK_PATH)
/* Takes ownership of font on both success and failure. */
static folio_font_t* font_finish_builtin(folio_font_t* font, int pixel_height) {
    uint32_t cmap = 0;
    if (!validate_ttf(font->bytes, font->length, &cmap) ||
        !stbtt_InitFont(&font->info, font->bytes, 0)) {
        folio_font_close(font);
        return NULL;
    }
    font->info.index_map = (int)cmap;
    font->info.kern = font->info.gpos = 0;
    font->cmap_segments = be16(font->bytes + cmap + 6) / 2;
    if (!folio_font_set_size(font, pixel_height)) { folio_font_close(font); return NULL; }
    return font;
}

static folio_font_t* font_open_deflated(const uint8_t* data, size_t total, size_t raw_size,
                                       int pixel_height, bool check_crc, uint32_t raw_crc) {
    if(!data || !total || raw_size<12 || raw_size>FOLIO_FONT_MAX_BYTES || pixel_height<12 || pixel_height>72)return NULL;
    folio_font_t* font=font_alloc_state();if(!font)return NULL;
    font->length=raw_size;
    // One bounded guard byte lets the decoder consume a final empty block and
    // distinguish exact-size output from an over-expanding stream.
    font->bytes=font_alloc(raw_size+1);
    if(!font->bytes){folio_font_close(font);return NULL;}
    size_t consumed=0,produced=0;bool okay=false;
#ifdef ESP_PLATFORM
    tinfl_decompressor* inflater=font_alloc(sizeof(*inflater));
    if(!inflater){folio_font_close(font);return NULL;}
    tinfl_init(inflater);
#else
    z_stream inflater={0};
    if(inflateInit2(&inflater,-15)!=Z_OK){folio_font_close(font);return NULL;}
#endif
    while(!font_cancelled()) {
        size_t input=total-consumed,output=raw_size+1-produced;
        if(output>32768)output=32768;
        size_t offered_input=input,offered_output=output;
#ifdef ESP_PLATFORM
        int status=tinfl_decompress(inflater,data+consumed,&input,font->bytes,font->bytes+produced,&output,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        bool done=status==TINFL_STATUS_DONE,failed=status<0;
#else
        inflater.next_in=(Bytef*)(data+consumed);inflater.avail_in=(uInt)input;
        inflater.next_out=font->bytes+produced;inflater.avail_out=(uInt)output;
        int status=inflate(&inflater,Z_NO_FLUSH);
        input=offered_input-inflater.avail_in;output=offered_output-inflater.avail_out;
        bool done=status==Z_STREAM_END,failed=status!=Z_OK && status!=Z_BUF_ERROR && !done;
#endif
        if(input>offered_input || output>offered_output)break;
        consumed+=input;produced+=output;
        if(produced>raw_size)break;
        if(done){okay=produced==raw_size && consumed==total;break;}
        if(failed || (!input&&!output))break;
        font_yield();
    }
#ifdef ESP_PLATFORM
    free(inflater);
#else
    inflateEnd(&inflater);
#endif
    if(!okay || (check_crc && folio_font_pack_crc32(font->bytes,raw_size)!=raw_crc)) {
        folio_font_close(font);return NULL;
    }
    return font_finish_builtin(font,pixel_height);
}
#endif

#if defined(ESP_PLATFORM) || defined(FOLIO_TEST_CARD_FONT)
/* Only these unmodified OFL assets can stand in for a built-in face. */
#include "folio_font_assets.h"

static folio_font_t* font_open_card_face(const char* id, int pixel_height) {
    size_t i = 0;
    for (; i < sizeof(card_faces) / sizeof(card_faces[0]); ++i) {
        if (!strcmp(id, card_faces[i].id)) break;
    }
    if (i == sizeof(card_faces) / sizeof(card_faces[0])) return NULL;
    FILE* file = fopen(card_faces[i].path, "rb");
    if (!file) return NULL;
    size_t length = card_faces[i].length;
    folio_font_t* font = NULL;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (fseek(file, 0, SEEK_END) == 0 && ftell(file) == (long)length &&
        fseek(file, 0, SEEK_SET) == 0 && psa_crypto_init() == PSA_SUCCESS &&
        psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS) {
        font = font_alloc_state();
    }
    if (font) {
        font->length = length;
        font->bytes = font_alloc(length);
    }
    bool ok = font && font->bytes;
    for (size_t pos = 0; ok && pos < length;) {
        size_t n = length - pos < 32768 ? length - pos : 32768;
        ok = !font_cancelled() && fread(font->bytes + pos, 1, n, file) == n &&
             psa_hash_update(&hash, font->bytes + pos, n) == PSA_SUCCESS;
        pos += n;
        font_yield();
    }
    uint8_t digest[32];
    size_t n = 0;
    ok = ok && fgetc(file) == EOF && !ferror(file) &&
         psa_hash_finish(&hash, digest, sizeof(digest), &n) == PSA_SUCCESS &&
         n == sizeof(digest) && !memcmp(digest, card_faces[i].sha256, n);
    psa_hash_abort(&hash);
    if (fclose(file) != 0 || !ok) { folio_font_close(font); return NULL; }
    return font_finish_builtin(font, pixel_height);
}
#endif

static folio_font_t* font_open_flash_resource(const char* id,int pixel_height) {
#if defined(ESP_PLATFORM) || defined(FOLIO_FONT_PACK_PATH)
    const size_t pack_size=0x500000u;
    const void* mapped=NULL;
#ifdef ESP_PLATFORM
    const esp_partition_t* partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_FAT,"storage");
    if(!partition || partition->address!=0x410000u || partition->size!=pack_size)return NULL;
    esp_partition_mmap_handle_t handle;
    if(esp_partition_mmap(partition,0,pack_size,ESP_PARTITION_MMAP_DATA,&mapped,&handle)!=ESP_OK)return NULL;
#else
    int fd=open(FOLIO_FONT_PACK_PATH,O_RDONLY);
    if(fd<0)return NULL;
    struct stat st;
    if(fstat(fd,&st)!=0 || st.st_size!=(off_t)pack_size){close(fd);return NULL;}
    mapped=mmap(NULL,pack_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
    if(mapped==MAP_FAILED)return NULL;
#endif
    folio_font_resource_t resource;
    folio_font_t* font=NULL;
    if(folio_font_pack_find(mapped,pack_size,id,&resource)) {
        font=font_open_deflated(resource.compressed,resource.compressed_size,resource.raw_size,
                               pixel_height,true,resource.raw_crc32);
        if(font && !strcmp(id,"yuan")) {
            if(folio_font_pack_find(mapped,pack_size,"symbols",&resource))
                font->fallback=font_open_deflated(resource.compressed,resource.compressed_size,resource.raw_size,
                                                 pixel_height,true,resource.raw_crc32);
            if(!font->fallback){folio_font_close(font);font=NULL;}
        }
    }
#ifdef ESP_PLATFORM
    esp_partition_munmap(handle);
#else
    munmap((void*)mapped,pack_size);
#endif
    return font;
#else
    (void)id;(void)pixel_height;return NULL;
#endif
}

// Index-only availability check for the font list: no decompression or hashing.
static bool font_resource_present(const char* id) {
#ifdef ESP_PLATFORM
    const esp_partition_t* partition=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_FAT,"storage");
    const void* mapped=NULL; esp_partition_mmap_handle_t handle;
    if(partition && partition->address==0x410000u && partition->size==0x500000u &&
       esp_partition_mmap(partition,0,0x500000u,ESP_PARTITION_MMAP_DATA,&mapped,&handle)==ESP_OK) {
        folio_font_resource_t resource;
        bool found=folio_font_pack_find(mapped,0x500000u,id,&resource);
        esp_partition_munmap(handle);
        if(found)return true;
    }
#endif
#if defined(ESP_PLATFORM) || defined(FOLIO_TEST_CARD_FONT)
    for(size_t i=0;i<sizeof(card_faces)/sizeof(card_faces[0]);++i) {
        struct stat info;
        if(!strcmp(id,card_faces[i].id))
            return stat(card_faces[i].path,&info)==0 && (size_t)info.st_size==card_faces[i].length &&
                   (strcmp(id,"yuan") || font_resource_present("symbols"));
    }
#endif
    (void)id; return false;
}
bool folio_font_builtin_installed(const char* path) {
    for(size_t i=0;i<sizeof(builtin_fonts)/sizeof(builtin_fonts[0]);++i)
        if(path && !strcmp(path,builtin_fonts[i].path))
            return !builtin_fonts[i].resource || font_resource_present(builtin_fonts[i].resource);
    return false;
}
static folio_font_t* font_open_resource(const char* id, int pixel_height) {
    folio_font_t* font = font_open_flash_resource(id, pixel_height);
#if defined(ESP_PLATFORM) || defined(FOLIO_TEST_CARD_FONT)
    if (!font && !font_cancelled()) {
        font = font_open_card_face(id, pixel_height);
        if (font && !strcmp(id, "yuan")) {
            font->fallback = font_open_card_face("symbols", pixel_height);
            if (!font->fallback) { folio_font_close(font); return NULL; }
        }
    }
#endif
    return font;
}

folio_font_t* folio_font_open_builtin(int pixel_height) {
#ifdef ESP_PLATFORM
    extern const uint8_t data_start[] asm("_binary_folio_builtin_deflate_start");
    extern const uint8_t data_end[] asm("_binary_folio_builtin_deflate_end");
    return font_open_deflated(data_start,(size_t)(data_end-data_start),FOLIO_BUILTIN_FONT_BYTES,pixel_height,false,0);
#elif defined(FOLIO_BUILTIN_FONT_PATH)
    return folio_font_open(FOLIO_BUILTIN_FONT_PATH,pixel_height);
#else
    (void)pixel_height;return NULL;
#endif
}
// Owns file on entry, including every failure path. Keep large outlines on SD;
// only the small layout tables are needed when measuring or paginating a book.
static void load_weight_axis(card_face_t* card,const uint8_t* dir,unsigned count,unsigned glyphs) {
    table_view_t f={0},g={0},avar={0};
    for(unsigned i=0;i<count;++i) {
        const uint8_t* t=dir+16*i;
        table_view_t v={NULL,be32(t+12),be32(t+8)};
        if(has_tag(t,"fvar")) f=v;
        if(has_tag(t,"gvar")) g=v;
        if(has_tag(t,"avar")) avar=v;
    }
    uint8_t h[20],axis[20];
    if(f.length<16 || g.length<20 || !card_read(card->file,f.offset,h,16) || be32(h)!=0x10000) return;
    unsigned axes=be16(h+8),step=be16(h+10),start=be16(h+4),weight=axes;
    if(!axes || axes>8 || step<20 || !contains_range(f.length,start,axes*step)) return;
    for(unsigned i=0;i<axes;++i) {
        if(!card_read(card->file,f.offset+start+i*step,axis,20)) return;
        if(!has_tag(axis,"wght")) continue;
        if(weight!=axes) return;
        int32_t min=(int32_t)be32(axis+4),def=(int32_t)be32(axis+8),max=(int32_t)be32(axis+12);
        if(min>def || def>max || min==max) return;
        weight=i;
        for(int j=-2;j<=2;++j) card->coords[j+2]=(j<0 && min==def)||(j>0 && max==def) ? 0 : j*8192;
    }
    if(weight==axes) return;
    if(avar.length) {
        if(avar.length<8 || !card_read(card->file,avar.offset,h,8) || be32(h)!=0x10000 || be16(h+6)!=axes) return;
        uint32_t pos=8;
        for(unsigned i=0;i<axes;++i) {
            if(!contains_range(avar.length,pos,2) || !card_read(card->file,avar.offset+pos,h,2)) return;
            unsigned pairs=be16(h);pos+=2;
            if(pairs<3 || pairs>256 || !contains_range(avar.length,pos,4*pairs)) return;
            if(i==weight) {
                float mapped[5]={0};
                int from=-16385,to=-16384;
                for(unsigned k=0;k<pairs;++k) {
                    if(!card_read(card->file,avar.offset+pos+4*k,h,4)) return;
                    int x=(int16_t)be16(h),y=(int16_t)be16(h+2);
                    if(x<=from || x>16384 || y < -16384 || y>16384 ||
                       (!k && (x!=-16384 || y!=-16384)) || (k+1==pairs && (x!=16384 || y!=16384))) return;
                    for(unsigned j=0;j<5;++j) if(card->coords[j]>from && card->coords[j]<=x)
                        mapped[j]=to+(card->coords[j]-from)*(y-to)/(x-from);
                    from=x;to=y;
                }
                memcpy(card->coords,mapped,sizeof(mapped));
            }
            pos+=pairs*4;
        }
    }
    if(!card_read(card->file,g.offset,h,20) || be32(h)!=0x10000 || be16(h+4)!=axes ||
       be16(h+12)!=glyphs || be16(h+6)>64 || be16(h+14)>1) return;
    unsigned shared=be16(h+6),stride=be16(h+14)?4:2;
    uint32_t begin=be32(h+16),peaks=be32(h+8);
    if (!contains_range(g.length,20,(glyphs+1)*stride) || begin<20+(glyphs+1)*stride ||
        begin>=g.length || !contains_range(g.length,peaks,2*shared*axes)) return;
    for(unsigned i=0;i<shared;++i) {
        uint8_t values[16];
        if(!card_read(card->file,g.offset+peaks+2*i*axes,values,2*axes)) return;
        for(unsigned j=0;j<axes;++j) card->tuple_peaks[i*axes+j]=be16(values+2*j);
    }
    card->axis_count=axes; card->weight_axis=weight;
    card->tuple_count=shared; card->wide_offsets=stride==4;card->var_offsets=g.offset+20;
    card->var_data=g.offset+begin;card->var_length=g.length-begin;
}

static folio_font_t* open_card_face(FILE* file, uint32_t length, int pixel_height) {
    enum { HEAD, MAXP, HHEA, HMTX, LOCA, CMAP, GLYF, TABLES };
    static const char tags[TABLES][5] = {"head", "maxp", "hhea", "hmtx", "loca", "cmap", "glyf"};
    static const uint32_t caps[GLYF] = {54, 6, 36, 65535u*4, 65536u*4, 512u*1024};
    table_view_t tables[TABLES] = {0};
    uint8_t header[12];
    uint8_t* directory = NULL;
    folio_font_t* font = font_alloc_state();
    if (!font || !(font->card = calloc(1, sizeof(*font->card)))) {
        fclose(file); folio_font_close(font); return NULL;
    }
    font->card->file = file;
    if (!card_read(file, 0, header, sizeof(header)) ||
        (be32(header) != 0x00010000 && be32(header) != 0x74727565)) goto fail;
    unsigned count = be16(header+4);
    if (count < TABLES || count > 64 || !(directory = malloc(count*16)) ||
        !card_read(file, 12, directory, count*16)) goto fail;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t* entry = directory + i*16;
        uint32_t offset = be32(entry+8), size = be32(entry+12);
        if (!contains_range(length, offset, size) || (size && offset < 12+count*16)) goto fail;
        for (unsigned j = 0; j < i; ++j) if (!memcmp(entry, directory+j*16, 4)) goto fail;
        for (unsigned j = 0; j < TABLES; ++j) if (has_tag(entry, tags[j])) {
            tables[j].length = size; tables[j].offset = offset;
        }
    }
    for (unsigned i = 0; i < GLYF; ++i) {
        if (!tables[i].length) goto fail;
        if (tables[i].length > caps[i]) {
            if (i == CMAP) goto fail;
            tables[i].length = caps[i];
        }
        font->length += tables[i].length;
    }
    font->bytes = font_alloc(font->length);
    if (!font->bytes) goto fail;
    size_t used = 0;
    for (unsigned i = 0; i < GLYF; ++i) {
        if (!card_read(file, tables[i].offset, font->bytes+used, tables[i].length)) goto fail;
        tables[i].data = font->bytes+used; tables[i].offset = (uint32_t)used; used += tables[i].length;
    }
    uint32_t cmap;
    if (!validate_metrics(tables[HEAD], tables[MAXP], tables[HHEA], tables[HMTX], tables[LOCA],
                          tables[GLYF], tables[CMAP], &cmap)) goto fail;
    font->cmap_segments = be16(font->bytes+cmap+6)/2;
    font->info = (stbtt_fontinfo){.data=font->bytes, .numGlyphs=be16(tables[MAXP].data+4),
        .index_map=(int)cmap, .hhea=(int)tables[HHEA].offset, .hmtx=(int)tables[HMTX].offset,
        .loca=(int)tables[LOCA].offset, .indexToLocFormat=be16(tables[HEAD].data+50)};
    load_weight_axis(font->card,directory,count,font->info.numGlyphs);
    free(directory); directory=NULL;
    font->card->glyf_offset = tables[GLYF].offset;
    font->card->glyf_length = tables[GLYF].length;
    // Opening/repagination must not scan megabytes of outlines. Check the
    // immutable index up front; glyph reads validate their own snapshots.
    uint32_t prev = 0;
    for (int i = 0; i <= font->info.numGlyphs; ++i) {
        uint32_t off;
        if (!read_loca(&tables[LOCA], font->info.indexToLocFormat != 0, (uint16_t)i, &off) ||
            off < prev || off > font->card->glyf_length) goto fail;
        prev = off;
    }
    if (!folio_font_set_size(font, pixel_height)) goto fail;
    return font;
fail:
    free(directory); folio_font_close(font); return NULL;
}

folio_font_t* folio_font_open(const char* path, int pixel_height) {
    if (path == NULL || pixel_height < 12 || pixel_height > 72) return NULL;
    if(folio_font_is_builtin_path(path)) {
        if(!strcmp(path,FOLIO_FONT_BUILTIN_PATH)) {
            // The in-app Songti is a 7.5k-character subset; books routinely use
            // characters outside it, which would otherwise render as '?'. Borrow
            // missing glyphs from an installed complete face (hei is the smallest).
            folio_font_t* font=folio_font_open_builtin(pixel_height);
            if(font && !font->fallback && !font_cancelled()) {
                font->fallback=font_open_resource("hei",pixel_height);
                if(!font->fallback)font->fallback=font_open_resource("kai",pixel_height);
            }
            return font;
        }
        for(unsigned i=1;i<FOLIO_FONT_BUILTIN_COUNT;++i)
            if(!strcmp(path,builtin_fonts[i].path))return font_open_resource(builtin_fonts[i].resource,pixel_height);
        return NULL;
    }
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length < 12 || (unsigned long)length > FOLIO_FONT_MAX_FILE_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    bool paged = (unsigned long)length > CARD_RESIDENT_LIMIT;
#if !defined(ESP_PLATFORM) && defined(FOLIO_BUILTIN_FONT_PATH)
    // The host simulator's application-resident Song face has no SD dependency.
    const char* bundled = FOLIO_BUILTIN_FONT_PATH;
    if (bundled && !strcmp(path, bundled)) paged = false;
#endif
    if (!paged) {
        uint8_t sfnt[12],entry[16];
        if (fread(sfnt,1,12,file)==12 && be16(sfnt+4)<=64)
            for(unsigned i=0;i<be16(sfnt+4) && fread(entry,1,16,file)==16;++i)
                if(has_tag(entry,"gvar")) {paged=true;break;}
        rewind(file);
    }
    if (paged) return open_card_face(file, (uint32_t)length, pixel_height);
    if ((unsigned long)length > FOLIO_FONT_MAX_BYTES) { fclose(file); return NULL; }
    folio_font_t* font = font_alloc_state();
    if (font == NULL) { fclose(file); return NULL; }
    font->length = (size_t)length;
    font->bytes = font_alloc(font->length);
    bool read_ok=font->bytes!=NULL; size_t read=0;
    while(read_ok && read<font->length) {
        size_t count=font->length-read<65536?font->length-read:65536;
        if(font_cancelled() || fread(font->bytes+read,1,count,file)!=count)read_ok=false;
        else read+=count;
    }
    if(fclose(file)!=0)read_ok=false;
    if (!read_ok) {
        folio_font_close(font);
        return NULL;
    }

    uint32_t cmap4_offset = 0;
    if (!validate_ttf(font->bytes, font->length, &cmap4_offset) ||
        stbtt_InitFont(&font->info, font->bytes, 0) == 0) {
        folio_font_close(font);
        return NULL;
    }
    font->info.index_map = (int)cmap4_offset;
    font->info.kern = 0;
    font->info.gpos = 0;
    font->cmap_segments = be16(font->bytes + cmap4_offset + 6) / 2;
    if (!folio_font_set_size(font, pixel_height)) { folio_font_close(font); return NULL; }
    return font;
}

bool folio_font_validate_file(const char* path) {
    if (!path || folio_font_is_builtin_path(path)) return false;
    folio_font_t* font = folio_font_open(path, 32);
    if (!font) return false;
    bool valid = true;
    if (font->card) for (int i = 0; i < font->info.numGlyphs; ++i) {
        stbtt_fontinfo snapshot;
        if ((i & 127) == 0) { if (font_cancelled()) { valid = false; break; } font_yield(); }
        if (!card_glyph(font, (uint16_t)i, &snapshot)) { valid = false; break; }
    }
    folio_font_close(font);
    return valid;
}

folio_font_t* folio_font_open_first_on_card(int pixel_height, char* path_out, uint32_t path_cap) {
    DIR* dir = opendir(FOLIO_CARD_FONT_DIR);
    if (dir == NULL) return NULL;
    struct dirent* item;
    folio_font_t* font = NULL;
    while ((item = readdir(dir)) != NULL) {
        const char* dot = strrchr(item->d_name, '.');
        if (dot == NULL || strcasecmp(dot, ".ttf") != 0) continue;
        char path[256];
        int written = snprintf(path, sizeof(path), "%s/%s", FOLIO_CARD_FONT_DIR, item->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        font = folio_font_open(path, pixel_height);
        if (font != NULL) {
            if (path_out != NULL && path_cap > 0) {
                strncpy(path_out, path, path_cap - 1);
                path_out[path_cap - 1] = '\0';
            }
            break;
        }
    }
    closedir(dir);
    return font;
}

void folio_font_close(folio_font_t* font) {
    if (font == NULL) return;
    folio_font_close(font->fallback);
    for (unsigned i = 0; i < font->cache_slots; ++i) {
        if (font->cache[i].pixels != NULL && font->cache[i].pixels != (uint8_t*)font) free(font->cache[i].pixels);
    }
    if (font->card) {
        fclose(font->card->file);
        free(font->card->tree);
        free(font->card);
    }
    if (font->cache!=font->local_cache) free(font->cache);
    free(font->advances);
    free(font->bytes);
    free(font);
}

bool folio_font_set_size(folio_font_t* font, int pixel_height) {
    if (font == NULL || pixel_height < 12 || pixel_height > 144) return false;
    if (font->pixel_height == pixel_height) return true;
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    if (ascent - descent < 16) return false;
    // CJK fonts disagree widely on hhea ascender/descender whitespace.
    // Normalize a representative ideograph ink box, rather than that whitespace.
    if(font->visual_height==0) {
        const uint32_t samples[]={0x56FD,0x9605,0x6C38,0x95F4,0x4E66};
        int total=0,top=0,count=0;
        for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);++i) {
            int gid=font_glyph_index(font,samples[i]),x0,y0,x1,y1;
            stbtt_fontinfo snapshot;
            stbtt_fontinfo* info = &font->info;
            if (!gid) continue;
            if (font->card) {
                if (!card_glyph(font, gid, &snapshot)) return false;
                info = &snapshot; gid = 0;
            }
            if(stbtt_GetGlyphBox(info,gid,&x0,&y0,&x1,&y1) && y1-y0>=16) {
                total+=y1-y0;top+=y1;++count;
            }
        }
        if(count>=3){font->visual_height=(float)total/count/0.78f;font->visual_top=(float)top/count;}
        else {font->visual_height=(float)(ascent-descent);font->visual_top=(float)ascent;}
    }
    float scale = (float)pixel_height / font->visual_height;
    if (!isfinite(scale) || scale <= 0) return false;
    if(font->fallback && !folio_font_set_size(font->fallback,pixel_height))return false;
    font->scale = scale;
    font->pixel_height = pixel_height;
    font->ascent = (int)lroundf(font->visual_top * scale);
    font->line_height = pixel_height;
    return true;
}

int folio_font_ascent(const folio_font_t* font) { return font != NULL ? font->ascent : 0; }
int folio_font_line_height(const folio_font_t* font) { return font != NULL ? font->line_height : 0; }
int folio_font_pixel_height(const folio_font_t* font) { return font != NULL ? font->pixel_height : 0; }

bool folio_font_has_glyph(folio_font_t* font, uint32_t cp) {
    return font && font_glyph_index(glyph_source(font, cp), cp) != 0;
}
static int font_advance_uncached(folio_font_t* font, uint32_t cp);
// Layout asks for the same few thousand advances on every page; each lookup is
// two cmap binary searches in PSRAM, so memoize them per pixel height.
int folio_font_advance(folio_font_t* font, uint32_t cp, uint32_t previous) {
    (void)previous; // Kerning and shaping are outside the supported cmap4/glyf subset.
    if (font == NULL) return 0;
    if (cp > 0xFFFF || font->pixel_height <= 0 || font->pixel_height > 255) return font_advance_uncached(font, cp);
    if (!font->advances) {
        font->advances = font_alloc(2 * ADVANCE_CACHE_N * sizeof(uint32_t));
        if (font->advances) memset(font->advances, 0xFF, 2 * ADVANCE_CACHE_N * sizeof(uint32_t));
        else return font_advance_uncached(font, cp);
    }
    uint32_t key = cp << 8 | (uint32_t)font->pixel_height;
    uint32_t* slot = &font->advances[2 * ((cp * 2654435761u >> 20) % ADVANCE_CACHE_N)];
    if (slot[0] == key) return (int)(int32_t)slot[1];
    int advance = font_advance_uncached(font, cp);
    slot[0] = key; slot[1] = (uint32_t)advance;
    return advance;
}
static int font_advance_uncached(folio_font_t* font, uint32_t cp) {
    folio_font_t* source=glyph_source(font,cp);
    int glyph_index = font_glyph_index(source, cp);
#if defined(ESP_PLATFORM) && defined(CONFIG_FOLIO_USB_DEBUG) && CONFIG_FOLIO_USB_DEBUG
    if (glyph_index == 0 && cp >= 0x80) {
        static uint32_t seen[64]; static unsigned count;
        bool known = false;
        for (unsigned i = 0; i < count && !known; ++i) known = seen[i] == cp;
        if (!known && count < 64) { seen[count++] = cp; printf("glyph missing U+%04lX\n", (unsigned long)cp); }
    }
#endif
    if (glyph_index == 0) {source=font;glyph_index=font_glyph_index(font, '?');}
    if (glyph_index == 0) return font->pixel_height;
    uint16_t metric_count = be16(source->bytes + source->info.hhea + 34);
    uint32_t metric = (uint32_t)(glyph_index < metric_count ? glyph_index : metric_count - 1);
    uint16_t width = be16(source->bytes + source->info.hmtx + metric * 4);
    return (int)lroundf(width * source->scale);
}

bool folio_font_glyph_bounds(folio_font_t* font, uint32_t cp,
                             int* left, int* top, int* right, int* bottom) {
    if (left) *left = 0;
    if (top) *top = 0;
    if (right) *right = 0;
    if (bottom) *bottom = 0;
    if (font == NULL) return false;
    glyph_cache_entry_t* entry = glyph_get(font, cp);
    if (entry == NULL) entry = glyph_get(font, '?');
    if (entry == NULL || !entry->ink_right || !entry->ink_bottom) return false;
    if (left) *left = entry->xoff + entry->ink_left;
    if (top) *top = entry->yoff + entry->ink_top;
    if (right) *right = entry->xoff + entry->ink_right;
    if (bottom) *bottom = entry->yoff + entry->ink_bottom;
    return true;
}

bool folio_font_draw_glyph(folio_font_t* font, uint8_t* framebuffer, int x, int baseline_y, uint32_t cp) {
    return folio_font_draw_glyph_color(font, framebuffer, x, baseline_y, cp, 0x00, 0xF0);
}

bool folio_font_draw_glyph_color(folio_font_t* font, uint8_t* framebuffer, int x, int baseline_y,
                                 uint32_t cp, uint8_t color, uint8_t background) {
    if (font == NULL || framebuffer == NULL) return false;
    glyph_cache_entry_t* entry = glyph_get(font, cp);
    if (entry == NULL) {
        entry = glyph_get(font, '?');
        if (entry == NULL) return false;
    }
    const uint8_t* pixels = entry->pixels == (uint8_t*)font ? NULL : entry->pixels;
    if (pixels == NULL) return true;
    for (int row = 0; row < entry->height; ++row) {
        for (int col = 0; col < entry->width; ++col) {
            uint8_t alpha = pixels[(size_t)row * entry->width + col];
            if (alpha == 0) continue;
            uint8_t shade = (uint8_t)(((uint16_t)alpha * (color >> 4) +
                (uint16_t)(255 - alpha) * (background >> 4) + 127) / 255);
            epd_draw_pixel(x + entry->xoff + col, baseline_y + entry->yoff + row,
                           (uint8_t)(shade << 4), framebuffer);
        }
    }
    return true;
}

int folio_font_draw_utf8(folio_font_t* font, uint8_t* framebuffer, int x, int baseline_y,
                         const char* text, uint8_t color) {
    if (font == NULL || framebuffer == NULL || text == NULL) return x;
    const char* p = text;
    uint32_t previous = 0;
    while (*p) {
        uint32_t cp = folio_utf8_next(&p);
        if (cp == '\n') break;
        (void)folio_font_draw_glyph_color(font, framebuffer, x, baseline_y, cp, color, 0xF0);
        x += folio_font_advance(font, cp, previous);
        previous = cp;
    }
    return x;
}
