// Asset-only raster equivalence: the tiny subset must render like unchanged Songti.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/book/folio_font.c"

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* framebuffer) {
    (void)x; (void)y; (void)color; (void)framebuffer;
}

int main(int argc, char** argv) {
    assert(argc == 4);
    FILE* chars = fopen(argv[3], "r");
    assert(chars);
    uint32_t cps[64]; unsigned count = 0, cp;
    while (fscanf(chars, "U+%x\n", &cp) == 1) {
        assert(count < 64); cps[count++] = cp;
    }
    assert(!ferror(chars) && fclose(chars) == 0 && count == 43);
    folio_font_t* songti = folio_font_open(argv[1], 24);
    folio_font_t* symbols = folio_font_open(argv[2], 24);
    assert(songti && symbols);
    const int sizes[] = {12, 24, 36, 48, 72, 96, 144};
    unsigned compared = 0;
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(*sizes); ++s) {
        assert(folio_font_set_size(songti, sizes[s]));
        assert(folio_font_set_size(symbols, sizes[s]));
        assert(songti->scale == symbols->scale);
        assert(songti->visual_height == symbols->visual_height);
        assert(songti->visual_top == symbols->visual_top);
        assert(folio_font_ascent(songti) == folio_font_ascent(symbols));
        for (unsigned i = 0; i < count; ++i) {
            assert(font_glyph_index(songti, cps[i]) && font_glyph_index(symbols, cps[i]));
            assert(folio_font_advance(songti, cps[i], 0) == folio_font_advance(symbols, cps[i], 0));
            glyph_cache_entry_t* a = glyph_get(songti, cps[i]);
            glyph_cache_entry_t* b = glyph_get(symbols, cps[i]);
            assert(a && b && a->width == b->width && a->height == b->height);
            assert(a->xoff == b->xoff && a->yoff == b->yoff);
            size_t bytes = (size_t)a->width * a->height;
            assert(!bytes || !memcmp(a->pixels, b->pixels, bytes));
            ++compared;
        }
    }
    printf("{\"symbol_glyphs\":%u,\"sizes\":7,\"raster_comparisons\":%u,\"scale_advance_bitmap_differences\":0,\"host_state_bytes\":%zu}\n",
           count, compared, sizeof(*symbols));
    folio_font_close(symbols); folio_font_close(songti);
    return 0;
}
