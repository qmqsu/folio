// Bounded parser regression cases run under AddressSanitizer/UndefinedBehaviorSanitizer.
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../main/book/folio_font.c"

static unsigned draw_count;
static bool capture_colors;
static uint8_t drawn_colors[1024];
static unsigned color_count;
static bool capture_ink;
static unsigned ink_count;
static int ink_left, ink_top, ink_right, ink_bottom;
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* framebuffer) {
    (void)framebuffer;
    assert(x > -1000 && x < 1000 && y > -1000 && y < 1000);
    ++draw_count;
    if (capture_ink && (color >> 4) < 15) {
        if (x < ink_left) ink_left = x;
        if (y < ink_top) ink_top = y;
        if (x + 1 > ink_right) ink_right = x + 1;
        if (y + 1 > ink_bottom) ink_bottom = y + 1;
        ++ink_count;
    }
    if (capture_colors) {
        assert(color_count < sizeof(drawn_colors));
        drawn_colors[color_count++] = color;
    }
}

static void check_glyph_bounds(folio_font_t* font, uint32_t cp) {
    int left = 99, top = 99, right = 99, bottom = 99;
    int advance = folio_font_advance(font, cp, 0);
    bool visible = folio_font_glyph_bounds(font, cp, &left, &top, &right, &bottom);
    assert(folio_font_advance(font, cp, 0) == advance);
    capture_ink = true; ink_count = 0;
    ink_left = ink_top = INT_MAX; ink_right = ink_bottom = INT_MIN;
    uint8_t buffer[1] = {0};
    (void)folio_font_draw_glyph(font, buffer, 50, 100, cp);
    capture_ink = false;
    assert(visible == (ink_count != 0));
    if (visible) {
        assert(left == ink_left - 50 && right == ink_right - 50);
        assert(top == ink_top - 100 && bottom == ink_bottom - 100);
        assert(right > left && bottom > top);
    } else assert(left == 0 && top == 0 && right == 0 && bottom == 0);
    assert(folio_font_glyph_bounds(font, cp, NULL, NULL, NULL, NULL) == visible);
}

static void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

typedef struct {
    uint8_t data[1024];
    uint32_t head, hhea, cmap, glyf, loca;
    size_t length;
} fixture_t;

static uint32_t add_table(fixture_t* f, unsigned index, const char* tag, uint32_t length) {
    uint32_t offset = (uint32_t)((f->length + 3) & ~(size_t)3);
    uint8_t* entry = f->data + 12 + index * 16;
    memcpy(entry, tag, 4);
    put32(entry + 8, offset);
    put32(entry + 12, length);
    f->length = offset + length;
    return offset;
}

static void square(uint8_t* p) {
    put16(p, 1);
    put16(p + 6, 100); put16(p + 8, 100);
    put16(p + 10, 3);
    memset(p + 14, 1, 4);
    put16(p + 20, 100); put16(p + 24, (uint16_t)-100);
    put16(p + 30, 100);
}

static fixture_t make_font(void) {
    fixture_t f = {.length = 12 + 9 * 16};
    put32(f.data, 0x00010000); put16(f.data + 4, 9);
    f.head = add_table(&f, 0, "head", 54);
    put16(f.data + f.head + 18, 1000); put16(f.data + f.head + 50, 1);
    uint32_t maxp = add_table(&f, 1, "maxp", 6);
    put16(f.data + maxp + 4, 3);
    f.hhea = add_table(&f, 2, "hhea", 36);
    put16(f.data + f.hhea + 4, 800); put16(f.data + f.hhea + 6, (uint16_t)-200);
    put16(f.data + f.hhea + 34, 3);
    uint32_t hmtx = add_table(&f, 3, "hmtx", 12);
    put16(f.data + hmtx, 500); put16(f.data + hmtx + 4, 500); put16(f.data + hmtx + 8, 700);
    f.loca = add_table(&f, 4, "loca", 16);
    put32(f.data + f.loca + 8, 34); put32(f.data + f.loca + 12, 68);
    f.glyf = add_table(&f, 5, "glyf", 68);
    square(f.data + f.glyf); square(f.data + f.glyf + 34);
    f.cmap = add_table(&f, 6, "cmap", 54);
    uint8_t* c = f.data + f.cmap;
    put16(c + 2, 1); put16(c + 4, 3); put16(c + 6, 1); put32(c + 8, 12);
    c += 12;
    put16(c, 4); put16(c + 2, 42); put16(c + 6, 6);
    put16(c + 8, 4); put16(c + 10, 1); put16(c + 12, 2);
    put16(c + 14, '?'); put16(c + 16, 'A'); put16(c + 18, 0xFFFF);
    put16(c + 22, '?'); put16(c + 24, 'A'); put16(c + 26, 0xFFFF);
    put16(c + 28, (uint16_t)(1 - '?')); put16(c + 30, 1); put16(c + 32, 1);
    put16(c + 36, 4); put16(c + 40, 1);
    // These deliberately truncated optional tables must never be consumed.
    (void)add_table(&f, 7, "kern", 1);
    (void)add_table(&f, 8, "GPOS", 1);
    return f;
}

static folio_font_t* open_fixture(const fixture_t* fixture, char path[128]) {
    snprintf(path, 128, "/tmp/folio-font-case-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE* file = fdopen(fd, "wb");
    assert(file != NULL);
    assert(fwrite(fixture->data, 1, fixture->length, file) == fixture->length);
    assert(fclose(file) == 0);
    return folio_font_open(path, 34);
}

static void rejects(fixture_t* fixture) {
    char path[128];
    folio_font_t* font = open_fixture(fixture, path);
    assert(font == NULL);
    assert(unlink(path) == 0);
}

static void font_regressions(void) {
    fixture_t valid = make_font();
    char path[128];
    folio_font_t* font = open_fixture(&valid, path);
    assert(font != NULL);
    assert(font_glyph_index(font, 'A') == 2); // Nonzero idDelta on an indirect map.
    assert(folio_font_advance(font, 'A', 'V') == 24);
    assert(font->tick == 0); // Measuring must not allocate/rasterize/cache bitmaps.
    assert(folio_font_advance(font, '?', 0) == 17);
    assert(folio_font_advance(font, 0x110000, 0) == 17);
    uint8_t framebuffer[1] = {0};
    assert(folio_font_draw_glyph(font, framebuffer, 10, 80, 'A'));
    assert(draw_count > 0 && font->tick > 0);
    glyph_cache_entry_t* small = glyph_get(font, 'A');
    uint8_t* small_pixels = small->pixels;
    check_glyph_bounds(font, 'A');
    check_glyph_bounds(font, 0x110000);
    assert(small->pixels == small_pixels);
    uint32_t cached_tick = font->tick;
    assert(folio_font_set_size(font, 34) && font->tick == cached_tick);
    assert(folio_font_set_size(font, 68));
    assert(font->tick == cached_tick && folio_font_advance(font, 'A', 0) == 48);
    glyph_cache_entry_t* large = glyph_get(font, 'A');
    assert(large != small && large->width > small->width && large->height > small->height);
    assert(folio_font_set_size(font, 34));
    assert(glyph_get(font, 'A') == small && small->pixels == small_pixels);
    assert(folio_font_set_size(font, 68) && glyph_get(font, 'A') == large);
    assert(!folio_font_set_size(font, 0));
    assert(folio_font_pixel_height(font) == 68);

    // Color is independent of the cached coverage. Reversing paper and ink must
    // reverse each antialiased pixel, and a gray foreground must keep its value.
    capture_colors = true; color_count = 0;
    assert(folio_font_draw_glyph(font, framebuffer, 10, 80, 'A'));
    unsigned black_count = color_count;
    uint8_t black_colors[1024]; memcpy(black_colors, drawn_colors, color_count);
    color_count = 0;
    assert(folio_font_draw_glyph_color(font, framebuffer, 10, 80, 'A', 0xF0, 0x00));
    assert(color_count == black_count);
    bool has_solid_ink = false, has_edge = false;
    for (unsigned i = 0; i < color_count; ++i) {
        assert(black_colors[i] + drawn_colors[i] == 0xF0);
        has_solid_ink |= black_colors[i] == 0;
        has_edge |= black_colors[i] > 0 && black_colors[i] < 0xF0;
    }
    assert(has_solid_ink && has_edge);
    color_count = 0;
    assert(folio_font_draw_glyph_color(font, framebuffer, 10, 80, 'A', 0x60, 0xF0));
    assert(color_count == black_count);
    for (unsigned i = 0; i < color_count; ++i) {
        assert(drawn_colors[i] >= 0x60 && drawn_colors[i] <= 0xF0);
        if (black_colors[i] == 0) assert(drawn_colors[i] == 0x60);
    }
    uint8_t gray_colors[1024]; memcpy(gray_colors, drawn_colors, color_count);
    color_count = 0;
    assert(folio_font_draw_utf8(font, framebuffer, 10, 80, "A", 0x60) == 58);
    assert(color_count == black_count && memcmp(gray_colors, drawn_colors, color_count) == 0);
    capture_colors = false;
    assert(folio_font_set_size(font, 144));
    assert(folio_font_pixel_height(font) == 144);
    check_glyph_bounds(font, 'A');
    assert(!folio_font_set_size(font, 145) && folio_font_pixel_height(font) == 144);
    // Exercise eviction with more glyph/size pairs than slots; closing frees
    // bitmaps from every retained size under ASan rather than only the last size.
    for (int size = 12; size <= 72; ++size) {
        assert(folio_font_set_size(font, size));
        assert(folio_font_draw_glyph(font, framebuffer, 10, 80, 'A'));
        assert(folio_font_draw_glyph(font, framebuffer, 10, 80, '?'));
        check_glyph_bounds(font, 'A');
    }
    folio_font_close(font);
    assert(unlink(path) == 0);

    fixture_t bad = valid;
    put16(bad.data + bad.cmap + 12 + 8, 0); // Corrupt cmap binary-search accelerator.
    rejects(&bad);
    bad = valid;
    put16(bad.data + bad.cmap + 12 + 40, 2); // Mapped glyph becomes numGlyphs.
    rejects(&bad);
    bad = valid;
    put16(bad.data + bad.cmap + 12 + 36, 0xFFFE); // Pointer outside cmap.
    rejects(&bad);
    bad = valid;
    put16(bad.data + bad.hhea + 4, 0); put16(bad.data + bad.hhea + 6, 0);
    rejects(&bad); // Prevent division by zero/infinite scale.
    bad = valid;
    put16(bad.data + bad.glyf + 10, 0); bad.data[bad.glyf + 14] = 0;
    rejects(&bad); // One-point off-curve contour previously read beyond vertices.
    bad = valid;
    put16(bad.data + bad.glyf + 20, 32000);
    rejects(&bad); // Coordinates no longer fit the declared allocation bounds.
    bad = valid;
    uint8_t* g = bad.data + bad.glyf + 34;
    memset(g, 0, 34); put16(g, 0xFFFF); put16(g + 10, 3); put16(g + 12, 2);
    rejects(&bad); // Compound glyph references itself.
    bad = valid;
    put32(bad.data + bad.loca + 12, 69);
    rejects(&bad); // loca's final offset escapes glyf.
    bad = valid;
    bad.length = 11;
    rejects(&bad);

    // Acyclic repeated empty components still multiply the renderer's work.
    // Three levels of 64 children must be rejected before they can be rendered.
    uint8_t compound_data[3 * 522] = {0}, loca_data[20] = {0};
    for (unsigned glyph = 1; glyph <= 3; ++glyph) {
        uint8_t* compound = compound_data + (glyph - 1) * 522;
        put16(compound, 0xFFFF);
        for (unsigned child = 0; child < 64; ++child) {
            put16(compound + 10 + child * 8, child == 63 ? 3 : 0x23);
            put16(compound + 12 + child * 8, (uint16_t)(glyph - 1));
        }
        put32(loca_data + (glyph + 1) * 4, glyph * 522);
        assert(validate_composite_glyph(compound, 522, 4));
    }
    table_view_t loca = {loca_data, sizeof(loca_data), 0};
    table_view_t glyf = {compound_data, sizeof(compound_data), 0};
    uint8_t states[4] = {0}, depths[4] = {0};
    uint16_t radii[4] = {0};
    uint32_t costs[4] = {0};
    assert(!validate_component_tree(&loca, &glyf, true, 4, 3, states, costs, depths, radii, 0));

    // Mutations exercise mixed header/range/outline damage under the sanitizers.
    uint32_t random = 0xC0FFEEu;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        bad = valid;
        for (unsigned flip = 0; flip <= attempt % 4; ++flip) {
            random = random * 1664525u + 1013904223u;
            uint32_t offset = random % (uint32_t)bad.length;
            random = random * 1664525u + 1013904223u;
            bad.data[offset] ^= (uint8_t)(1u << (random % 8));
        }
        font = open_fixture(&bad, path);
        if (font != NULL) {
            for (uint32_t cp = 32; cp < 128; ++cp) {
                (void)folio_font_advance(font, cp, 'V');
                (void)folio_font_draw_glyph(font, framebuffer, 10, 80, cp);
            }
            folio_font_close(font);
        }
        assert(unlink(path) == 0);
    }

    puts("font: render, grayscale/inverse text, multi-size cache, idDelta, measurement, bounded compounds and 1000 mutations passed");
}

static FILE* bytes_file(const uint8_t* bytes, size_t count) {
    FILE* file = tmpfile();
    assert(file != NULL && fwrite(bytes, 1, count, file) == count);
    rewind(file);
    return file;
}

static void text_regressions(void) {
    folio_encoding_t encoding;
    long length;
    uint32_t cp;
    const uint8_t bom_bad[] = {0xEF, 0xBB, 0xBF, 'A', 0xFF, 'B'};
    FILE* file = bytes_file(bom_bad, sizeof(bom_bad));
    assert(folio_text_detect(file, &encoding, &length));
    assert(encoding == FOLIO_ENCODING_UTF8 && length == 6 && ftell(file) == 3);
    assert(folio_text_next(file, encoding, &cp) && cp == 'A');
    assert(folio_text_next(file, encoding, &cp) && cp == 0xFFFD);
    assert(folio_text_next(file, encoding, &cp) && cp == 'B');
    assert(!folio_text_next(file, encoding, &cp) && !ferror(file));
    fclose(file);

    const uint8_t utf16[] = {0xFF, 0xFE, 'A', 0};
    file = bytes_file(utf16, sizeof(utf16));
    assert(!folio_text_detect(file, &encoding, &length)); fclose(file);
    const uint8_t gbk[] = {0xD6, 0xD0, 0xCE, 0xC4, 0x81, 0x20};
    file = bytes_file(gbk, sizeof(gbk));
    assert(folio_text_detect(file, &encoding, &length) && encoding == FOLIO_ENCODING_GBK);
    assert(folio_text_next(file, encoding, &cp) && cp == 0x4E2D);
    assert(folio_text_next(file, encoding, &cp) && cp == 0x6587);
    assert(folio_text_next(file, encoding, &cp) && cp == 0xFFFD);
    assert(folio_text_next(file, encoding, &cp) && cp == ' '); fclose(file);
    const uint8_t literal_replacement[] = {0xEF, 0xBF, 0xBD};
    file = bytes_file(literal_replacement, sizeof(literal_replacement));
    assert(folio_text_detect(file, &encoding, &length) && encoding == FOLIO_ENCODING_UTF8);
    fclose(file);
    file = tmpfile(); assert(file != NULL);
    assert(folio_text_detect(file, &encoding, &length) && length == 0);
    fclose(file);

    const char malformed[] = "\xED\xA0\x80";
    const char* p = malformed;
    assert(folio_utf8_next(&p) == 0xFFFD && p == malformed + 1);
    const char truncated[] = "\xF0\x9F";
    p = truncated;
    assert(folio_utf8_next(&p) == 0xFFFD && p == truncated + 1);
    file = tmpfile(); assert(file != NULL);
    setvbuf(file, NULL, _IONBF, 0);
    assert(close(fileno(file)) == 0);
    assert(!folio_text_next(file, FOLIO_ENCODING_UTF8, &cp) && ferror(file));
    (void)fclose(file);
    puts("text: BOM, malformed bytes, GBK, Unicode scalar, EOF and I/O cases passed");
}

int main(int argc, char** argv) {
    font_regressions();
    text_regressions();
    int absent[4] = {1, 1, 1, 1};
    assert(!folio_font_glyph_bounds(NULL, 'A', &absent[0], &absent[1], &absent[2], &absent[3]));
    assert(absent[0] == 0 && absent[1] == 0 && absent[2] == 0 && absent[3] == 0);
    for (int i = 1; i < argc; ++i) {
        folio_font_t* font = folio_font_open(argv[i], 34);
        if (font == NULL) { fprintf(stderr, "font rejected: %s\n", argv[i]); return 1; }
        uint8_t framebuffer[1] = {0};
        for (uint32_t cp = 32; cp < 256; ++cp) {
            assert(folio_font_advance(font, cp, 0) >= 0);
            (void)folio_font_draw_glyph(font, framebuffer, 10, 100, cp);
        }
        const int sizes[] = {12, 22, 32, 48, 72, 144, 22};
        const uint32_t glyphs[] = {'A', 'g', 'j', '0', '1', '8', '9', ':', '.', '+', '-', '%', ' ', 0x3000, 0x4E2D, 0x9605, 0x8BFB, 0x10FFFF};
        for (unsigned si = 0; si < sizeof(sizes) / sizeof(*sizes); ++si) {
            assert(folio_font_set_size(font, sizes[si]));
            for (unsigned ci = 0; ci < sizeof(glyphs) / sizeof(*glyphs); ++ci) check_glyph_bounds(font, glyphs[ci]);
            int l, t, r, b;
            assert(!folio_font_glyph_bounds(font, ' ', &l, &t, &r, &b));
            assert(l == 0 && t == 0 && r == 0 && b == 0);
            int ql, qt, qr, qb;
            bool question = folio_font_glyph_bounds(font, '?', &ql, &qt, &qr, &qb);
            assert(folio_font_glyph_bounds(font, 0x10FFFF, &l, &t, &r, &b) == question);
            assert(l == ql && t == qt && r == qr && b == qb);
        }
        folio_font_close(font);
        printf("system-font render and exact visible-ink bounds at 7 sizes (including 144 px) passed: %s\n", argv[i]);
    }
    return 0;
}
