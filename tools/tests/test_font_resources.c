/* Real loader integration; hooks affect this translation unit only. */
#include <stdbool.h>
#include <stddef.h>

#ifdef FONT_RESOURCE_CANCEL_HOOK
extern size_t font_test_cancel_calls;
extern size_t font_test_cancel_at;
bool folio_io_cancelled(void)
{
    return ++font_test_cancel_calls >= font_test_cancel_at;
}
#else

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define IMAGE_BYTES ((size_t)5242880)
#define HEADER_BYTES ((size_t)4096)
#define ENTRY_OFFSET(i) ((size_t)64 + (size_t)(i) * 128)
#define FRAME_SIDE 384
#define FRAME_BYTES ((size_t)FRAME_SIDE * FRAME_SIDE)

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

bool fail_crypto_init, fail_hash_setup, fail_hash_finish;
static const char *test_pack_path;
static const char *test_songti_path;
size_t font_test_cancel_calls;
size_t font_test_cancel_at = SIZE_MAX;
static void *tracked_malloc(size_t size);
static void *tracked_calloc(size_t count, size_t size);
static void tracked_free(void *pointer);
static FILE *tracked_fopen(const char *path, const char *mode);
static int tracked_fclose(FILE *file);
static int tracked_open(const char *path, int flags, ...);
static int tracked_close(int descriptor);
static void *tracked_mmap(void *address, size_t length, int protection,
                          int flags, int descriptor, off_t offset);
static int tracked_munmap(void *address, size_t length);
static int tracked_inflate_init(z_streamp stream, int bits,
                                const char *version, int stream_size);

#define FOLIO_BUILTIN_FONT_PATH test_songti_path
#define FOLIO_FONT_PACK_PATH test_pack_path
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
#define fopen tracked_fopen
#define fclose tracked_fclose
#define open tracked_open
#define close tracked_close
#define mmap tracked_mmap
#define munmap tracked_munmap
#define inflateInit2_ tracked_inflate_init
#include "../../main/book/folio_font.c"
#undef malloc
#undef calloc
#undef free
#undef fopen
#undef fclose
#undef open
#undef close
#undef mmap
#undef munmap
#undef inflateInit2_

static struct { void *pointer; size_t size; } allocations[1024];
static size_t live_allocations, live_bytes, allocation_calls, allocation_fail_at = SIZE_MAX;
static size_t allocation_sizes[65536];
static size_t allocation_failures;
static unsigned file_calls, live_files, live_descriptors, live_mappings;
static bool fail_mapping;
static const char *const resource_ids[] = {"hei", "kai", "yuan", "symbols"};
static const char *const native_paths[] = {":builtin:", ":builtin:hei", ":builtin:kai", ":builtin:yuan"};
static uint8_t frame_a[FRAME_BYTES], frame_b[FRAME_BYTES];

static void *tracked_malloc(size_t size)
{
    CHECK(allocation_calls < sizeof(allocation_sizes) / sizeof(*allocation_sizes));
    allocation_sizes[allocation_calls++] = size;
    if (allocation_calls >= allocation_fail_at) {
        ++allocation_failures;
        return NULL;
    }
    void *pointer = malloc(size);
    if (pointer == NULL) return NULL;
    for (size_t i = 0; i < sizeof(allocations) / sizeof(*allocations); ++i) {
        if (allocations[i].pointer == NULL) {
            allocations[i].pointer = pointer;
            allocations[i].size = size;
            ++live_allocations;
            live_bytes += size;
            return pointer;
        }
    }
    CHECK(false);
    return NULL;
}

static void *tracked_calloc(size_t count, size_t size)
{
    if (count != 0 && size > SIZE_MAX / count) return NULL;
    void *pointer = tracked_malloc(count * size);
    if (pointer != NULL) memset(pointer, 0, count * size);
    return pointer;
}

static void tracked_free(void *pointer)
{
    if (pointer == NULL) return;
    for (size_t i = 0; i < sizeof(allocations) / sizeof(*allocations); ++i) {
        if (allocations[i].pointer == pointer) {
            CHECK(live_allocations != 0 && live_bytes >= allocations[i].size);
            --live_allocations;
            live_bytes -= allocations[i].size;
            allocations[i].pointer = NULL;
            allocations[i].size = 0;
            free(pointer);
            return;
        }
    }
    CHECK(false); /* Foreign/double free. */
}

static FILE *tracked_fopen(const char *path, const char *mode)
{
    ++file_calls;
    FILE *file = fopen(path, mode);
    if (file != NULL) ++live_files;
    return file;
}

static int tracked_fclose(FILE *file)
{
    CHECK(live_files != 0);
    --live_files;
    return fclose(file);
}

static int tracked_open(const char *path, int flags, ...)
{
    CHECK(flags == O_RDONLY);
    ++file_calls;
    int descriptor = open(path, flags);
    if (descriptor >= 0) ++live_descriptors;
    return descriptor;
}

static int tracked_close(int descriptor)
{
    CHECK(live_descriptors != 0);
    --live_descriptors;
    return close(descriptor);
}

static void *tracked_mmap(void *address, size_t length, int protection,
                          int flags, int descriptor, off_t offset)
{
    if (fail_mapping) { errno = ENOMEM; return MAP_FAILED; }
    void *result = mmap(address, length, protection, flags, descriptor, offset);
    if (result != MAP_FAILED) ++live_mappings;
    return result;
}

static int tracked_munmap(void *address, size_t length)
{
    CHECK(live_mappings != 0);
    int result = munmap(address, length);
    CHECK(result == 0);
    --live_mappings;
    return result;
}

static voidpf tracked_zalloc(voidpf opaque, uInt count, uInt size)
{
    (void)opaque;
    return tracked_calloc(count, size);
}

static void tracked_zfree(voidpf opaque, voidpf pointer)
{
    (void)opaque;
    tracked_free(pointer);
}

static int tracked_inflate_init(z_streamp stream, int bits,
                                const char *version, int stream_size)
{
    stream->zalloc = tracked_zalloc;
    stream->zfree = tracked_zfree;
    return inflateInit2_(stream, bits, version, stream_size);
}

static void check_clean(void)
{
    CHECK(live_allocations == 0 && live_bytes == 0);
    CHECK(live_files == 0 && live_descriptors == 0 && live_mappings == 0);
}

static void reset_hooks(void)
{
    check_clean();
    allocation_calls = allocation_failures = 0;
    allocation_fail_at = SIZE_MAX;
    font_test_cancel_calls = 0;
    font_test_cancel_at = SIZE_MAX;
    file_calls = 0;
    fail_mapping = false;
}

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t *framebuffer)
{
    CHECK(x >= 0 && x < FRAME_SIDE && y >= 0 && y < FRAME_SIDE);
    CHECK((color & 15) == 0);
    framebuffer[(size_t)y * FRAME_SIDE + (size_t)x] = color;
}

static void draw_at_text_top(folio_font_t *font, uint32_t codepoint, uint8_t *framebuffer)
{
    memset(framebuffer, 0xf0, FRAME_BYTES);
    CHECK(folio_font_draw_glyph(font, framebuffer, 64,
                                64 + folio_font_ascent(font), codepoint));
}

static uint32_t get_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static void seal_header(uint8_t *image)
{
    put_le32(image + 44, 0);
    put_le32(image + 44, (uint32_t)crc32(0, image, (uInt)HEADER_BYTES));
}

static void seal_payload(uint8_t *image, unsigned index)
{
    uint8_t *entry = image + ENTRY_OFFSET(index);
    uint32_t offset = get_le32(entry + 16), length = get_le32(entry + 20);
    CHECK(offset <= IMAGE_BYTES && length <= IMAGE_BYTES - offset);
    put_le32(entry + 28, (uint32_t)crc32(0, image + offset, length));
    uint32_t last = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *current = image + ENTRY_OFFSET(i);
        uint32_t end = get_le32(current + 16) + get_le32(current + 20);
        if (end > last) last = end;
    }
    put_le32(image + 16, (last + 4095) & ~UINT32_C(4095));
    seal_header(image);
}

static void write_pack(const uint8_t *bytes, size_t length)
{
    check_clean();
    FILE *file = fopen(test_pack_path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, length, file) == length);
    CHECK(fclose(file) == 0);
}

static uint8_t *read_pack(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    uint8_t *image = malloc(IMAGE_BYTES + 1);
    CHECK(image != NULL);
    CHECK(fread(image, 1, IMAGE_BYTES + 1, file) == IMAGE_BYTES);
    CHECK(!ferror(file) && fclose(file) == 0);
    return image;
}

static size_t read_codepoints(const char *path, uint32_t *out, size_t capacity)
{
    FILE *file = fopen(path, "r");
    CHECK(file != NULL);
    size_t count = 0;
    unsigned codepoint;
    while (fscanf(file, "U+%x\n", &codepoint) == 1) {
        CHECK(count < capacity);
        out[count++] = codepoint;
    }
    CHECK(feof(file) && !ferror(file) && fclose(file) == 0);
    return count;
}

static void test_registry(void)
{
    reset_hooks();
    CHECK(!folio_font_is_builtin_path(NULL));
    CHECK(!folio_font_is_builtin_path("/sdcard/fonts/a.ttf"));
    CHECK(folio_font_builtin_path(FOLIO_FONT_BUILTIN_COUNT) == NULL);
    CHECK(folio_font_builtin_path(UINT_MAX) == NULL);
    CHECK(folio_font_builtin_label(NULL) == NULL);
    const char *bad[] = {
        ":builtin:missing", ":builtin:symbols", ":builtin:HEI", ":builtin:hei/../kai",
        ":builtin:../file.ttf", ":builtin:hei.ttf", ":builtin:hei ", ":builtin::hei"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        CHECK(folio_font_is_builtin_path(bad[i]));
        CHECK(folio_font_builtin_label(bad[i]) == NULL);
        CHECK(folio_font_open(bad[i], 24) == NULL);
        CHECK(file_calls == 0); /* Not even a failed fopen/open is allowed. */
    }
    CHECK(folio_font_open(NULL, 24) == NULL);
    CHECK(folio_font_open(":builtin:hei", 11) == NULL);
    CHECK(folio_font_open(":builtin:hei", 73) == NULL);
    CHECK(file_calls == 0);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(strcmp(folio_font_builtin_path(i), native_paths[i]) == 0);
        CHECK(folio_font_builtin_label(native_paths[i]) != NULL);
        folio_font_t *font = folio_font_open(native_paths[i], 24);
        CHECK(font != NULL);
        /* Songti borrows missing glyphs from hei; yuan carries its symbols face. */
        CHECK((font->fallback != NULL) == (i == 0 || i == 3));
        if (i == 0) CHECK(folio_font_has_glyph(font, 0x5C34) && folio_font_has_glyph(font, 0x5C2C));
        CHECK(live_files == 0 && live_descriptors == 0 && live_mappings == 0);
        folio_font_close(font);
        check_clean();
    }
    for (unsigned i = 0; i < 4; ++i) {
        folio_font_t *font = font_open_resource(resource_ids[i], 24);
        CHECK(font != NULL);
        folio_font_close(font);
        check_clean();
    }
    CHECK(font_test_cancel_calls != 0); /* Strong hook overrides the weak core hook. */
    puts("registry: four selectable IDs, four resources, reserved-ID filesystem isolation passed");
}

static void compare_fallback(folio_font_t *yuan, folio_font_t *songti, uint32_t cp)
{
    CHECK(font_glyph_index(yuan, cp) == 0);
    CHECK(glyph_source(yuan, cp) == yuan->fallback);
    CHECK(font_glyph_index(yuan->fallback, cp) != 0);
    CHECK(font_glyph_index(songti, cp) != 0);
    CHECK(yuan->fallback->scale == songti->scale);
    CHECK(folio_font_advance(yuan, cp, 0) == folio_font_advance(songti, cp, 0));
    glyph_cache_entry_t *actual = glyph_get(yuan, cp);
    glyph_cache_entry_t *expected = glyph_get(songti, cp);
    CHECK(actual != NULL && expected != NULL);
    CHECK(actual->width == expected->width && actual->height == expected->height);
    CHECK(actual->xoff == expected->xoff);
    CHECK(actual->yoff + yuan->ascent == expected->yoff + songti->ascent);
    size_t bytes = (size_t)actual->width * (size_t)actual->height;
    CHECK(bytes == 0 || memcmp(actual->pixels, expected->pixels, bytes) == 0);
    int a[4], b[4];
    bool actual_ink = folio_font_glyph_bounds(yuan, cp, a, a + 1, a + 2, a + 3);
    bool expected_ink = folio_font_glyph_bounds(songti, cp, b, b + 1, b + 2, b + 3);
    CHECK(actual_ink == expected_ink);
    if (actual_ink) {
        CHECK(a[0] == b[0] && a[2] == b[2]);
        CHECK(a[1] + yuan->ascent == b[1] + songti->ascent);
        CHECK(a[3] + yuan->ascent == b[3] + songti->ascent);
    }
    draw_at_text_top(yuan, cp, frame_a);
    draw_at_text_top(songti, cp, frame_b);
    CHECK(memcmp(frame_a, frame_b, FRAME_BYTES) == 0);
}

static void compare_question(folio_font_t *font, uint32_t cp)
{
    CHECK(font_glyph_index(font, cp) == 0);
    CHECK(font->fallback == NULL || font_glyph_index(font->fallback, cp) == 0);
    CHECK(folio_font_advance(font, cp, 0) == folio_font_advance(font, '?', 0));
    int a[4], b[4];
    bool actual = folio_font_glyph_bounds(font, cp, a, a + 1, a + 2, a + 3);
    bool expected = folio_font_glyph_bounds(font, '?', b, b + 1, b + 2, b + 3);
    CHECK(actual == expected && memcmp(a, b, sizeof(a)) == 0);
    draw_at_text_top(font, cp, frame_a);
    draw_at_text_top(font, '?', frame_b);
    CHECK(memcmp(frame_a, frame_b, FRAME_BYTES) == 0);
}

static void test_coverage_and_fallback(const uint32_t *coverage, size_t count,
                                       const uint32_t *fallback, size_t fallback_count)
{
    reset_hooks();
    folio_font_t *fonts[4];
    for (unsigned i = 0; i < 4; ++i) {
        fonts[i] = folio_font_open(native_paths[i], 24);
        CHECK(fonts[i] != NULL);
    }
    size_t yuan_missing = 0;
    for (size_t i = 0; i < count; ++i) {
        for (unsigned face = 1; face < 4; ++face) {
            CHECK(font_glyph_index(glyph_source(fonts[face], coverage[i]), coverage[i]) != 0);
        }
        if (!font_glyph_index(fonts[3], coverage[i])) ++yuan_missing;
    }
    CHECK(yuan_missing == fallback_count && fallback_count == 38);
    const int sizes[] = {12, 24, 36, 48, 72, 96, 144};
    const uint32_t absent[] = {0x0378, 0xd800, 0xffff, 0x10000, 0x10ffff, 0x110000, UINT32_MAX};
    unsigned raster_comparisons = 0;
    for (size_t size = 0; size < sizeof(sizes) / sizeof(*sizes); ++size) {
        for (unsigned i = 0; i < 4; ++i) CHECK(folio_font_set_size(fonts[i], sizes[size]));
        CHECK(fonts[3]->fallback->pixel_height == sizes[size]);
        for (size_t i = 0; i < fallback_count; ++i) {
            compare_fallback(fonts[3], fonts[0], fallback[i]);
            ++raster_comparisons;
        }
        for (unsigned face = 0; face < 4; ++face) {
            for (size_t i = 0; i < sizeof(absent) / sizeof(*absent); ++i)
                compare_question(fonts[face], absent[i]);
        }
    }
    for (unsigned i = 0; i < 4; ++i) folio_font_close(fonts[i]);
    check_clean();
    printf("coverage: %zu codepoints x Hei/Kai/Yuan+Symbols; %u exact Songti fallback comparisons\n",
           count, raster_comparisons);
}

static void test_cache_cycles(const uint32_t *coverage, uint32_t fallback_cp)
{
    for (unsigned cycle = 0; cycle < 12; ++cycle) {
        reset_hooks();
        folio_font_t *font = folio_font_open(native_paths[1 + cycle % 3], 24);
        CHECK(font != NULL);
        uint32_t cp = font->fallback ? fallback_cp : 0x4e66;
        glyph_cache_entry_t *small = glyph_get(font, cp);
        CHECK(small != NULL);
        uint8_t *small_pixels = small->pixels;
        size_t current_bytes = live_bytes;
        CHECK(glyph_get(font, cp) == small && live_bytes == current_bytes);
        CHECK(folio_font_set_size(font, 48));
        glyph_cache_entry_t *large = glyph_get(font, cp);
        CHECK(large != NULL && large != small);
        CHECK(folio_font_set_size(font, 24));
        CHECK(glyph_get(font, cp) == small && small->pixels == small_pixels);
        CHECK(!folio_font_set_size(font, 11) && font->pixel_height == 24);
        CHECK(!folio_font_set_size(font, 145) && font->pixel_height == 24);
        for (unsigned i = 0; i < 160; ++i) CHECK(glyph_get(font, coverage[100 + i]) != NULL);
        unsigned cached = 0;
        for (unsigned i = 0; i < FOLIO_GLYPH_CACHE_N; ++i)
            if (font->cache[i].pixels != NULL) ++cached;
        CHECK(cached == FOLIO_GLYPH_CACHE_N);
        CHECK(glyph_get(font, cp) != NULL); /* Eviction did not corrupt the fallback. */
        folio_font_close(font);
        check_clean();
    }
    puts("cache: cross-size reuse, 96-slot eviction and 12 open/close cycles have zero live allocations");
}

static void expect_bad_pack(const uint8_t *image, size_t length, const char *id,
                            bool parser_accepts)
{
    write_pack(image, length);
    reset_hooks();
    if (parser_accepts) {
        folio_font_resource_t resource;
        CHECK(folio_font_pack_find(image, length, id, &resource));
    }
    CHECK(font_open_resource(id, 24) == NULL);
    check_clean();
}

static void replace_stream(uint8_t *image, unsigned index, const uint8_t *raw,
                           size_t raw_length, bool final_empty_block)
{
    size_t capacity = compressBound((uLong)raw_length) + 256;
    uint8_t *compressed = malloc(capacity);
    CHECK(compressed != NULL);
    z_stream stream = {0};
    CHECK(deflateInit2(&stream, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    stream.next_in = (Bytef *)raw;
    stream.avail_in = (uInt)raw_length;
    stream.next_out = compressed;
    stream.avail_out = (uInt)capacity;
    if (final_empty_block) CHECK(deflate(&stream, Z_SYNC_FLUSH) == Z_OK);
    CHECK(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    size_t compressed_length = stream.total_out;
    CHECK(deflateEnd(&stream) == Z_OK);
    uint8_t *entry = image + ENTRY_OFFSET(index);
    uint32_t offset = get_le32(entry + 16);
    uint32_t limit = index == 3 ? (uint32_t)IMAGE_BYTES - 65536 :
                     get_le32(image + ENTRY_OFFSET(index + 1) + 16);
    CHECK(offset < limit && compressed_length <= limit - offset);
    memset(image + offset, 0xff, limit - offset);
    memcpy(image + offset, compressed, compressed_length);
    put_le32(entry + 20, (uint32_t)compressed_length);
    put_le32(entry + 24, (uint32_t)raw_length);
    put_le32(entry + 32, (uint32_t)crc32(0, raw, (uInt)raw_length));
    seal_payload(image, index);
    free(compressed);
}

static void test_corruption(const uint8_t *original)
{
    uint8_t *image = malloc(IMAGE_BYTES + 1);
    CHECK(image != NULL);
    reset_hooks();
    const char *saved_path = test_pack_path;
    char missing[PATH_MAX];
    CHECK(snprintf(missing, sizeof(missing), "%s.missing", saved_path) > 0);
    test_pack_path = missing;
    CHECK(font_open_resource("hei", 24) == NULL);
    check_clean();
    test_pack_path = saved_path;
    write_pack(original, IMAGE_BYTES);
    fail_mapping = true;
    CHECK(font_open_resource("hei", 24) == NULL);
    check_clean();
    reset_hooks();

    const size_t wrong_sizes[] = {0, HEADER_BYTES - 1, IMAGE_BYTES - 1, IMAGE_BYTES + 1};
    memcpy(image, original, IMAGE_BYTES);
    image[IMAGE_BYTES] = 0;
    for (size_t i = 0; i < sizeof(wrong_sizes) / sizeof(*wrong_sizes); ++i)
        expect_bad_pack(image, wrong_sizes[i], "hei", false);

    memcpy(image, original, IMAGE_BYTES);
    image[0] ^= 1;
    expect_bad_pack(image, IMAGE_BYTES, "hei", false);
    memcpy(image, original, IMAGE_BYTES);
    image[HEADER_BYTES + 1] ^= 1;
    expect_bad_pack(image, IMAGE_BYTES, "hei", false);
    memcpy(image, original, IMAGE_BYTES);
    image[get_le32(image + ENTRY_OFFSET(0) + 16)] ^= 1;
    expect_bad_pack(image, IMAGE_BYTES, "hei", false);

    /* CRC-repaired mutations must reach the loader, not just the container. */
    memcpy(image, original, IMAGE_BYTES);
    put_le32(image + ENTRY_OFFSET(0) + 32, get_le32(image + ENTRY_OFFSET(0) + 32) ^ 1);
    seal_header(image);
    expect_bad_pack(image, IMAGE_BYTES, "hei", true);
    memcpy(image, original, IMAGE_BYTES);
    put_le32(image + ENTRY_OFFSET(3) + 32, get_le32(image + ENTRY_OFFSET(3) + 32) ^ 1);
    seal_header(image);
    expect_bad_pack(image, IMAGE_BYTES, "yuan", true); /* Primary succeeds; fallback fails. */

    const int size_delta[] = {-1, 1};
    for (size_t i = 0; i < sizeof(size_delta) / sizeof(*size_delta); ++i) {
        memcpy(image, original, IMAGE_BYTES);
        uint8_t *entry = image + ENTRY_OFFSET(0);
        put_le32(entry + 24, (uint32_t)((int)get_le32(entry + 24) + size_delta[i]));
        seal_header(image);
        expect_bad_pack(image, IMAGE_BYTES, "hei", true);
    }
    memcpy(image, original, IMAGE_BYTES);
    put_le32(image + ENTRY_OFFSET(0) + 24, FOLIO_FONT_MAX_BYTES + 1);
    seal_header(image);
    expect_bad_pack(image, IMAGE_BYTES, "hei", false);

    memcpy(image, original, IMAGE_BYTES);
    uint8_t *entry = image + ENTRY_OFFSET(0);
    put_le32(entry + 20, get_le32(entry + 20) - 1);
    seal_payload(image, 0);
    expect_bad_pack(image, IMAGE_BYTES, "hei", true); /* Truncated DEFLATE. */
    memcpy(image, original, IMAGE_BYTES);
    entry = image + ENTRY_OFFSET(0);
    image[get_le32(entry + 16) + get_le32(entry + 20)] = 0;
    put_le32(entry + 20, get_le32(entry + 20) + 1);
    seal_payload(image, 0);
    expect_bad_pack(image, IMAGE_BYTES, "hei", true); /* Trailing input. */
    memcpy(image, original, IMAGE_BYTES);
    image[get_le32(image + ENTRY_OFFSET(0) + 16)] = 7; /* Reserved DEFLATE block type. */
    seal_payload(image, 0);
    expect_bad_pack(image, IMAGE_BYTES, "hei", true);

    uint8_t invalid_ttf[12] = {0};
    memcpy(image, original, IMAGE_BYTES);
    replace_stream(image, 0, invalid_ttf, sizeof(invalid_ttf), false);
    expect_bad_pack(image, IMAGE_BYTES, "hei", true); /* Valid inflate, invalid font. */

    uint8_t *expansion = calloc(32768, 1);
    CHECK(expansion != NULL);
    memcpy(image, original, IMAGE_BYTES);
    replace_stream(image, 0, expansion, 32768, false);
    put_le32(image + ENTRY_OFFSET(0) + 24, 12);
    put_le32(image + ENTRY_OFFSET(0) + 32, (uint32_t)crc32(0, expansion, 12));
    seal_header(image);
    expect_bad_pack(image, IMAGE_BYTES, "hei", true); /* 32768 -> declared 12, bounded guard. */
    free(expansion);

    /* Exact output followed by an empty final block is valid, not over-expansion. */
    folio_font_resource_t symbols;
    CHECK(folio_font_pack_find(original, IMAGE_BYTES, "symbols", &symbols));
    uint8_t *raw = malloc(symbols.raw_size);
    CHECK(raw != NULL);
    z_stream inflater = {0};
    CHECK(inflateInit2(&inflater, -15) == Z_OK);
    inflater.next_in = (Bytef *)symbols.compressed;
    inflater.avail_in = (uInt)symbols.compressed_size;
    inflater.next_out = raw;
    inflater.avail_out = (uInt)symbols.raw_size;
    CHECK(inflate(&inflater, Z_FINISH) == Z_STREAM_END);
    CHECK(inflateEnd(&inflater) == Z_OK);
    memcpy(image, original, IMAGE_BYTES);
    replace_stream(image, 3, raw, symbols.raw_size, true);
    write_pack(image, IMAGE_BYTES);
    reset_hooks();
    folio_font_t *font = font_open_resource("symbols", 24);
    CHECK(font != NULL);
    folio_font_close(font);
    check_clean();
    free(raw);
    free(image);
    write_pack(original, IMAGE_BYTES);
    puts("corruption: missing/mmap/size/CRC/stream/over-expansion/trailing input/fallback failure cleanup passed");
}

static void test_fault_injection(void)
{
    unsigned allocation_cases = 0, cancellation_cases = 0;
    const char *targets[] = {"symbols", "yuan"};
    for (unsigned target = 0; target < 2; ++target) {
        reset_hooks();
        folio_font_t *font = font_open_resource(targets[target], 24);
        CHECK(font != NULL);
        size_t total_allocations = allocation_calls;
        size_t total_cancellations = font_test_cancel_calls;
        size_t secondary_state = 0;
        if (target == 1) {
            for (size_t i = 1; i < total_allocations; ++i)
                if (allocation_sizes[i] == sizeof(folio_font_t)) secondary_state = i + 1;
            CHECK(secondary_state > 1);
        }
        folio_font_close(font);
        check_clean();
        for (size_t failure = 1; failure <= total_allocations; ++failure) {
            bool test = target == 0 || failure <= 8 || failure + 8 > total_allocations ||
                        (failure & (failure - 1)) == 0 ||
                        (secondary_state > 0 && failure >= secondary_state - 2 && failure <= secondary_state + 6);
            if (!test) continue;
            reset_hooks();
            allocation_fail_at = failure; /* Both PSRAM and generic fallback fail. */
            CHECK(font_open_resource(targets[target], 24) == NULL);
            CHECK(allocation_failures != 0);
            check_clean();
            ++allocation_cases;
        }
        for (size_t failure = 1; failure <= total_cancellations; ++failure) {
            reset_hooks();
            font_test_cancel_at = failure;
            CHECK(font_open_resource(targets[target], 24) == NULL);
            CHECK(font_test_cancel_calls >= failure);
            check_clean();
            ++cancellation_cases;
        }
        reset_hooks();
        font = font_open_resource(targets[target], 24);
        CHECK(font != NULL); /* Failure/cancellation leaves no poisoned global state. */
        folio_font_close(font);
        check_clean();
    }
    printf("fault injection: %u allocation failpoints + %u cancellation points; zero leaks/maps/fds\n",
           allocation_cases, cancellation_cases);
}

static void test_card_only(const char* missing, const char* valid_pack)
{
    test_pack_path = missing;
    for (unsigned i = 1; i <= 3; ++i) {
        folio_font_t* font = folio_font_open(native_paths[i], 24);
        CHECK(font != NULL && (font->fallback != NULL) == (i == 3));
        CHECK(font_glyph_index(font, 0x4e66) != 0);
        folio_font_close(font);
        check_clean();
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset_hooks();
        fail_crypto_init = failure == 0;
        fail_hash_setup = failure == 1;
        fail_hash_finish = failure == 2;
        if (failure == 3) font_test_cancel_at = 4;
        if (failure == 4) allocation_fail_at = 2;
        CHECK(folio_font_open(native_paths[1], 24) == NULL);
        check_clean();
    }
    reset_hooks();
    FILE* file = fopen(card_faces[3].path, "r+b");
    CHECK(file != NULL && fseek(file, -1, SEEK_END) == 0);
    int byte = fgetc(file);
    CHECK(byte != EOF && fseek(file, -1, SEEK_END) == 0);
    CHECK(fputc(byte ^ 1, file) != EOF && fclose(file) == 0);
    CHECK(folio_font_open(native_paths[3], 24) == NULL); /* Same length, wrong SHA. */
    check_clean();
    file = fopen(card_faces[0].path, "wb");
    CHECK(file != NULL && fclose(file) == 0);
    CHECK(folio_font_open(native_paths[1], 24) == NULL);
    check_clean();
    test_pack_path = valid_pack;
    reset_hooks();
    folio_font_t* font = folio_font_open(native_paths[3], 24);
    CHECK(font && font->fallback && file_calls == 1); /* No card access. */
    folio_font_close(font);
    check_clean();
    printf("verified card fallback: 3 faces, Symbols, SHA/size/crypto/cancel/OOM failures, flash priority; %u checks\n", checks);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--card-only")) {
        test_card_only(argv[2], argv[3]);
        return 0;
    }
    if (argc != 6) {
        fprintf(stderr, "usage: %s songti.ttf pack.bin charset-B.txt fallback.txt private-temp-dir\n", argv[0]);
        return 2;
    }
    test_songti_path = argv[1];
    char pack_path[PATH_MAX];
    int written = snprintf(pack_path, sizeof(pack_path), "%s/working-font-pack.bin", argv[5]);
    CHECK(written > 0 && (size_t)written < sizeof(pack_path));
    test_pack_path = pack_path;
    uint8_t *original = read_pack(argv[2]);
    uint32_t *coverage = malloc(7545 * sizeof(*coverage));
    CHECK(coverage != NULL);
    uint32_t fallback[38];
    size_t count = read_codepoints(argv[3], coverage, 7545);
    size_t fallback_count = read_codepoints(argv[4], fallback, 38);
    CHECK(count == 7545 && fallback_count == 38);
    write_pack(original, IMAGE_BYTES);
    test_registry();
    test_coverage_and_fallback(coverage, count, fallback, fallback_count);
    test_cache_cycles(coverage, fallback[0]);
    test_corruption(original);
    test_fault_injection();
    free(coverage);
    free(original);
    check_clean();
    printf("font resource loader tests passed: %u checks\n", checks);
    return 0;
}
#endif
