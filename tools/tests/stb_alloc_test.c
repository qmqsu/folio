/* Exercise allocator exhaustion using an ordinary glyph, not malformed input. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int calls, fail_at, live; } alloc_state_t;

static void *test_alloc(size_t bytes, alloc_state_t *s)
{
    if (++s->calls == s->fail_at) return NULL;
    void *p = malloc(bytes);
    assert(p);
    ++s->live;
    return p;
}

static void test_free(void *p, alloc_state_t *s)
{
    if (p) --s->live;
    free(p);
}

#define STBTT_malloc(n,u) test_alloc((n),(u))
#define STBTT_free(p,u) test_free((p),(u))
#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long size = ftell(f);
    assert(size > 0);
    rewind(f);
    unsigned char *bytes = malloc((size_t)size);
    assert(bytes && fread(bytes, 1, (size_t)size, f) == (size_t)size);
    fclose(f);

    alloc_state_t s = {0};
    stbtt_fontinfo info = {0};
    info.userdata = &s;
    assert(stbtt_InitFont(&info, bytes, 0));
    int glyph = stbtt_FindGlyphIndex(&info, 'A');
    float scale = stbtt_ScaleForPixelHeight(&info, 24);
    assert(glyph);
    for (int sdf = 0; sdf <= 1; ++sdf) {
        int baseline = 0;
        for (int fail = 0; fail <= baseline; ++fail) {
            s = (alloc_state_t){.fail_at = fail};
            if (sdf) {
                unsigned char *p = stbtt_GetGlyphSDF(&info, scale, glyph, 2, 128, 16,
                                                      NULL, NULL, NULL, NULL);
                assert((p != NULL) == (fail == 0));
                stbtt_FreeSDF(p, &s);
            } else {
                /* Width > 512 takes the dynamic scanline path in both versions. */
                unsigned char out[513 * 32] = {0};
                stbtt_MakeGlyphBitmap(&info, out, 513, 32, 513, scale, scale, glyph);
                if (!fail) assert(memcmp(out, (unsigned char[sizeof out]){0}, sizeof out));
            }
            assert(s.live == 0 && s.calls >= fail);
            if (!fail) baseline = s.calls;
        }
        printf("rasterizer %d %s: %d allocation failures passed\n",
               STBTT_RASTERIZER_VERSION, sdf ? "SDF" : "bitmap", baseline);
    }
    free(bytes);
    return 0;
}
