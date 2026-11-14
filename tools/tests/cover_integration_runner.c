#include "folio_cover.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 684
#define SCREEN_H 1216
static uint8_t framebuffer[SCREEN_W * SCREEN_H];
static const EpdRect rect = {40, 50, 192, 288};

void* heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
int epd_rotated_display_width(void) { return SCREEN_W; }
int epd_rotated_display_height(void) { return SCREEN_H; }
void epd_draw_pixel(int x, int y, uint8_t gray, uint8_t* target) {
    assert(x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height);
    assert(gray % 17 == 0);
    target[(size_t)y * SCREEN_W + x] = gray;
}

static uint32_t draw(const char* path, bool expected) {
    memset(framebuffer, 42, sizeof(framebuffer));
    assert(folio_cover_draw(path, framebuffer, rect) == expected);
    size_t nonwhite = 0;
    uint32_t hash = UINT32_C(2166136261);
    for (int y = 0; y < SCREEN_H; ++y) for (int x = 0; x < SCREEN_W; ++x) {
        uint8_t gray = framebuffer[(size_t)y * SCREEN_W + x];
        bool inside = x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
        if (!expected || !inside) { assert(gray == 42); continue; }
        assert(gray % 17 == 0);
        nonwhite += gray < 255;
        hash = (hash ^ gray) * UINT32_C(16777619);
    }
    if (expected) assert(nonwhite > 128);
    return hash;
}

int main(int argc, char** argv) {
    assert(argc == 6);
    uint32_t hashes[3];
    for (int i = 0; i < 3; ++i) hashes[i] = draw(argv[i + 1], true);
    assert(hashes[0] != hashes[1] && hashes[1] != hashes[2] && hashes[0] != hashes[2]);
    (void)draw(argv[4], false);
    (void)draw(argv[5], false);
    // Revisit in a different order to catch accidental cross-book cache reuse.
    for (int i = 2; i >= 0; --i) assert(draw(argv[i + 1], true) == hashes[i]);
    folio_cover_clear_cache();
    for (int i = 0; i < 3; ++i) assert(draw(argv[i + 1], true) == hashes[i]);
    folio_cover_clear_cache();
    puts("real EPUB -> cover image -> decoder -> framebuffer passed: EPUB3 PNG, EPUB2 JPEG, guide SVG; corrupt/missing rejected; cache identity and bounds checked");
    return 0;
}
