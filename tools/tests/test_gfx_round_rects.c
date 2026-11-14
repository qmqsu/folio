#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "folio_gfx.h"

#define WIDTH 684
#define HEIGHT 1216
#define WHITE 255
#define INK 0x70
static uint8_t screen[WIDTH * HEIGHT], outer[WIDTH * HEIGHT], border[WIDTH * HEIGHT];
static EpdRect allowed;
static unsigned writes;
int epd_rotated_display_width(void) { return WIDTH; }
int epd_rotated_display_height(void) { return HEIGHT; }
void epd_draw_pixel(int x,int y,uint8_t color,uint8_t* fb) {
    assert(x>=0 && x<WIDTH && y>=0 && y<HEIGHT);
    assert((int64_t)x>=allowed.x && (int64_t)x<(int64_t)allowed.x+allowed.width);
    assert((int64_t)y>=allowed.y && (int64_t)y<(int64_t)allowed.y+allowed.height);
    assert(color==INK);
    fb[(size_t)y*WIDTH+x]=color; ++writes;
}
void epd_fill_rect(EpdRect r,uint8_t color,uint8_t* fb) {
    for(int y=r.y;y<r.y+r.height;y++)for(int x=r.x;x<r.x+r.width;x++)epd_draw_pixel(x,y,color,fb);
}
void epd_draw_rect(EpdRect r,uint8_t color,uint8_t* fb) {
    for(int y=r.y;y<r.y+r.height;y++)for(int x=r.x;x<r.x+r.width;x++)
        if(x==r.x || y==r.y || x==r.x+r.width-1 || y==r.y+r.height-1)epd_draw_pixel(x,y,color,fb);
}
static void clear(uint8_t* fb) { memset(fb,WHITE,sizeof(screen));writes=0; }
static bool marked(const uint8_t* fb,EpdRect r,int x,int y) { return fb[(size_t)(r.y+y)*WIDTH+r.x+x]==INK; }
static void symmetric(const uint8_t* fb,EpdRect r) {
    for(int y=0;y<r.height;y++)for(int x=0;x<r.width;x++) {
        bool value=marked(fb,r,x,y);
        assert(value==marked(fb,r,r.width-1-x,y));
        assert(value==marked(fb,r,x,r.height-1-y));
    }
}
static void check_case(int width,int height,int radius,int thickness) {
    EpdRect r={10,20,width,height};allowed=r;
    clear(outer);folio_gfx_fill_round_rect(outer,r,radius,INK);assert(writes>0);symmetric(outer,r);
    clear(border);folio_gfx_round_rect(border,r,radius,INK,thickness);assert(writes>0);symmetric(border,r);
    int side=width<height?width:height;
    int clamped=radius<0?0:radius>side/2?side/2:radius;
    // Clamping must produce the exact same footprint as a caller-clamped radius.
    clear(screen);folio_gfx_fill_round_rect(screen,r,clamped,INK);
    for(int y=0;y<height;y++)for(int x=0;x<width;x++)assert(marked(screen,r,x,y)==marked(outer,r,x,y));
    // The inward stroke and rounded interior partition the filled shape without
    // overwriting its background: a useful contract for selected UI controls.
    clear(screen);
    if(thickness<=(side-1)/2) {
        EpdRect inner={r.x+thickness,r.y+thickness,width-2*thickness,height-2*thickness};
        folio_gfx_fill_round_rect(screen,inner,clamped-thickness,INK);
    }
    for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
        bool fill=marked(outer,r,x,y),line=marked(border,r,x,y),inner=marked(screen,r,x,y);
        assert(!(line && inner)); assert(fill==(line || inner));
    }
}
static void explicit_mask(void) {
    const char* mask[]={"..####..",".######.","########","########","########","########",".######.","..####.."};
    allowed=(EpdRect){10,20,8,8};clear(screen);folio_gfx_fill_round_rect(screen,allowed,4,INK);
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)assert(marked(screen,allowed,x,y)==(mask[y][x]=='#'));
    // For radius zero the exact border includes every edge and no center pixel.
    clear(screen);allowed=(EpdRect){20,20,30,20};folio_gfx_round_rect(screen,allowed,0,INK,2);
    for(int y=0;y<20;y++)for(int x=0;x<30;x++)assert(marked(screen,allowed,x,y)==(x<2 || x>=28 || y<2 || y>=18));
}
int main(void) {
    const int sizes[]={1,2,3,4,7,8,13,24,25,73};
    const int radii[]={-1,0,1,4,12,INT_MAX};
    const int widths[]={1,2,4,12,INT_MAX};
    unsigned cases=0;
    for(unsigned wi=0;wi<sizeof(sizes)/sizeof(*sizes);wi++)
        for(unsigned hi=0;hi<sizeof(sizes)/sizeof(*sizes);hi++)
            for(unsigned ri=0;ri<sizeof(radii)/sizeof(*radii);ri++)
                for(unsigned ti=0;ti<sizeof(widths)/sizeof(*widths);ti++) {
                    check_case(sizes[wi],sizes[hi],radii[ri],widths[ti]);++cases;
                }
    explicit_mask();
    const EpdRect edges[]={{-15,-9,30,40},{WIDTH-15,HEIGHT-19,30,40},{-100,-100,20,20},
        {INT_MAX,INT_MAX,INT_MAX,INT_MAX},{INT_MIN,INT_MIN,INT_MAX,INT_MAX},{-10,-10,INT_MAX,INT_MAX}};
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);i++) {
        allowed=edges[i];clear(screen);folio_gfx_fill_round_rect(screen,allowed,12,INK);
        folio_gfx_round_rect(screen,allowed,INT_MAX,INK,2);
    }
    allowed=(EpdRect){10,10,20,20};clear(screen);
    folio_gfx_round_rect(screen,allowed,12,INK,0);folio_gfx_round_rect(screen,allowed,12,INK,-1);
    folio_gfx_fill_round_rect(NULL,allowed,12,INK);folio_gfx_round_rect(NULL,allowed,12,INK,1);
    folio_gfx_fill_round_rect(screen,(EpdRect){10,10,0,20},12,INK);
    folio_gfx_round_rect(screen,(EpdRect){10,10,20,-1},12,INK,1);assert(writes==0);
    printf("GFX rounded rectangles: %u fill/stroke combinations; symmetry, exact masks, inward borders, radius clamp, clipping and empty inputs passed\n",cases);
    return 0;
}
