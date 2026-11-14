#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "folio_gfx.h"

#define WIDTH 684
#define HEIGHT 400
static uint8_t screen[WIDTH * HEIGHT];
static unsigned outside;
int epd_rotated_display_width(void) { return WIDTH; }
int epd_rotated_display_height(void) { return HEIGHT; }
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) { ++outside; return; }
    fb[(size_t)y * WIDTH + x] = color;
}
void epd_fill_rect(EpdRect rect, uint8_t color, uint8_t* fb) {
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x) epd_draw_pixel(x, y, color, fb);
}
void epd_draw_rect(EpdRect r, uint8_t color, uint8_t* fb) {
    epd_fill_rect((EpdRect){r.x,r.y,r.width,1},color,fb);
    epd_fill_rect((EpdRect){r.x,r.y+r.height-1,r.width,1},color,fb);
    epd_fill_rect((EpdRect){r.x,r.y,1,r.height},color,fb);
    epd_fill_rect((EpdRect){r.x+r.width-1,r.y,1,r.height},color,fb);
}

int main(int argc, char** argv) {
    const int sizes[] = {8, 12, 24, 36, 48, 64, 128, 256};
    for (int icon = 0; icon < FOLIO_ICON_COUNT; ++icon) {
        for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
            memset(screen, 255, sizeof(screen));
            EpdRect rect = {20, 20, sizes[size], sizes[size]};
            folio_gfx_icon(screen, rect, (folio_icon_t)icon, 0x40);
            unsigned painted = 0;
            for (int y = 0; y < HEIGHT; ++y) for (int x = 0; x < WIDTH; ++x) {
                if (screen[(size_t)y * WIDTH + x] == 255) continue;
                assert(screen[(size_t)y * WIDTH + x] == 0x40);
                assert(x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height);
                ++painted;
            }
            assert(painted > 0);
        }
    }
    // Edge clipping must not rely on EPDiy's internals or write outside the buffer.
    folio_gfx_icon(screen, (EpdRect){-20, -20, 48, 48}, FOLIO_ICON_POWER, 0);
    folio_gfx_icon(screen, (EpdRect){WIDTH-20, HEIGHT-20, 48, 48}, FOLIO_ICON_BOOK, 0);
    folio_gfx_line(screen, -30, -30, 30, 30, 0, 4);
    folio_gfx_line(screen, WIDTH+20, HEIGHT+20, WIDTH-30, HEIGHT-30, 0, 3);
    folio_gfx_circle(screen, 0, 0, 30, 0, 3);
    folio_gfx_circle(screen, WIDTH-1, HEIGHT-1, 30, 0, 3);
    assert(outside == 0);

    // Case-sensitive Wi-Fi passwords and session URLs must remain readable
    // before a Chinese TTF has been installed.
    for(char letter='a';letter<='z';++letter) {
        uint8_t lower[35],upper[35];char value[2]={letter,0};
        memset(screen,255,sizeof(screen));folio_gfx_text(screen,20,20,1,value,0);
        for(int y=0;y<7;++y)for(int x=0;x<5;++x)lower[y*5+x]=screen[(20+y)*WIDTH+20+x];
        value[0]=(char)(letter-'a'+'A');memset(screen,255,sizeof(screen));folio_gfx_text(screen,20,20,1,value,0);
        for(int y=0;y<7;++y)for(int x=0;x<5;++x)upper[y*5+x]=screen[(20+y)*WIDTH+20+x];
        assert(memcmp(lower,upper,sizeof(lower))!=0);
    }

    if (argc == 2) {
        static const char* labels[] = {"BACK", "CHEVRON LEFT", "CHEVRON RIGHT", "GRID", "LIST", "BOOK", "BOOKMARK", "TYPE", "SETTINGS", "REFRESH", "POWER", "LOCK", "CHECK", "PLUS", "MINUS", "CLOSE", "HOME"};
        memset(screen, 255, sizeof(screen));
        for (int i = 0; i < FOLIO_ICON_COUNT; ++i) {
            int x = 16 + (i % 4) * 164, y = 12 + (i / 4) * 96;
            folio_gfx_icon(screen, (EpdRect){x+54,y,48,48}, (folio_icon_t)i, 0);
            folio_gfx_text(screen, x + (156 - (int)strlen(labels[i]) * 6) / 2, y + 64, 1, labels[i], 0);
        }
        FILE* file = fopen(argv[1], "wb"); assert(file != NULL);
        fprintf(file, "P5\n%d %d\n255\n", WIDTH, HEIGHT);
        assert(fwrite(screen, 1, sizeof(screen), file) == sizeof(screen)); assert(fclose(file) == 0);
        assert(outside == 0);
    }
    printf("GFX: %d icons at 8 sizes stay within their rectangles; edge strokes clip safely\n", FOLIO_ICON_COUNT);
    return 0;
}
