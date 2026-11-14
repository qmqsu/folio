#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "../../main/book/folio_cover.c"

static int fail_allocations;
static unsigned epub_calls, clipped;
static const char* fixture_root;
static uint8_t framebuffer[684*1216];
void* heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    if (fail_allocations) return NULL;
    return malloc(size);
}
int epd_rotated_display_width(void) { return 684; }
int epd_rotated_display_height(void) { return 1216; }
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* buffer) {
    if (x<0 || y<0 || x>=684 || y>=1216) { ++clipped; return; }
    assert(color % 17 == 0); buffer[(size_t)y*684+x]=color;
}
static uint8_t* read_fixture(const char* name, size_t* size) {
    char path[1024]; snprintf(path,sizeof(path),"%s/%s",fixture_root,name);
    return cover_read(path,size);
}
bool folio_epub_cover(const char* path, uint8_t** data, size_t* size, char* error, size_t capacity) {
    ++epub_calls; *data=NULL; *size=0; if (capacity) *error=0;
    if (strstr(path,"embedded") || strstr(path,"duplicate")) *data=read_fixture("rgba.png",size);
    return *data != NULL;
}
static void path_for(const char* name,char path[1024]) { snprintf(path,1024,"%s/%s",fixture_root,name); }
static void touch_book(const char* name) {
    char path[1024]; path_for(name,path); FILE* file=fopen(path,"ab"); assert(file); assert(fputc('x',file)!=EOF); assert(fclose(file)==0);
}
static void test_decode(const char* name, bool expected, unsigned wanted_w,unsigned wanted_h) {
    size_t length; uint8_t* bytes=read_fixture(name,&length); assert(bytes);
    unsigned w=0,h=0; uint8_t* result=cover_decode(bytes,length,&w,&h);
    assert((result != NULL)==expected);
    if (result) { assert(w==wanted_w && h==wanted_h); free(result); }
    free(bytes); assert(s_work_live==0);
    printf("cover decode: %s %s\n",name,expected?"accepted":"rejected");
}
static void copy_fixture(const char* from,const char* to) {
    size_t size; uint8_t* data=read_fixture(from,&size); assert(data);
    char path[1024];path_for(to,path);FILE*f=fopen(path,"wb");assert(f);assert(fwrite(data,1,size,f)==size);assert(fclose(f)==0);free(data);
}
static void test_cache_and_layout(void) {
    EpdRect rect={50,60,192,288}; char path[1024];
    path_for("embedded.epub",path);touch_book("embedded.epub");
    unsigned before=epub_calls;
    assert(folio_cover_draw(path,framebuffer,rect)); assert(folio_cover_draw(path,framebuffer,rect)); assert(epub_calls==before+1);
    touch_book("embedded.epub"); assert(folio_cover_draw(path,framebuffer,rect)); assert(epub_calls==before+2);
    path_for("duplicate.epub",path);touch_book("duplicate.epub");assert(folio_cover_draw(path,framebuffer,rect));
    unsigned images=0;for(unsigned i=0;i<COVER_IMAGES;++i)images+=s_images[i].pixels!=NULL;assert(images==1);
    path_for("absent.epub",path);touch_book("absent.epub");before=epub_calls;
    assert(!folio_cover_draw(path,framebuffer,rect));assert(!folio_cover_draw(path,framebuffer,rect));assert(epub_calls==before+1);
    copy_fixture("wide.png","absent.png");assert(folio_cover_draw(path,framebuffer,rect));assert(epub_calls==before+2);
    path_for("wide.txt",path);touch_book("wide.txt");
    memset(framebuffer,33,sizeof(framebuffer));assert(folio_cover_draw(path,framebuffer,rect));
    assert(framebuffer[60*684+50]==255); assert(framebuffer[204*684+146]==0);
    assert(framebuffer[59*684+50]==33 && framebuffer[348*684+50]==33);
    assert(folio_cover_draw(path,framebuffer,(EpdRect){1,1,1,1}));
    assert(folio_cover_draw(path,framebuffer,(EpdRect){1,1,500,20}));
    assert(folio_cover_draw(path,framebuffer,(EpdRect){1,1,20,500}));
    assert(!folio_cover_draw(path,framebuffer,(EpdRect){-1,0,20,20}));
    assert(!folio_cover_draw(path,framebuffer,(EpdRect){683,0,2,20}));
    path_for("jpeg.txt",path);touch_book("jpeg.txt");copy_fixture("large.jpg","jpeg.jpeg");assert(folio_cover_draw(path,framebuffer,rect));
    // A different book must not receive the prior book's cover or negative cache.
    path_for("unrelated.txt",path);touch_book("unrelated.txt");assert(!folio_cover_draw(path,framebuffer,rect));
    folio_cover_clear_cache();assert(!clipped && s_work_live==0);
}
static void test_wallpaper(void) {
    char path[1024];path_for("wallpaper.png",path);
    assert(folio_cover_draw_image(path,framebuffer,(EpdRect){0,0,684,1216}));
    // One-pixel vertical stripes prove that wallpaper bypasses cover thumbnails.
    for(unsigned y=0;y<1216;++y)for(unsigned x=0;x<684;++x)assert(framebuffer[y*684+x]==(x%2?255:0));
    path_for("truncated.png",path);memset(framebuffer,33,sizeof(framebuffer));
    assert(!folio_cover_draw_image(path,framebuffer,(EpdRect){0,0,684,1216}));
    for(size_t i=0;i<sizeof(framebuffer);++i)assert(framebuffer[i]==33);
    path_for("large.jpg",path);assert(folio_cover_draw_image(path,framebuffer,(EpdRect){0,0,684,1216}));
    assert(!folio_cover_draw_image(path,framebuffer,(EpdRect){0,0,685,1216}));
    fail_allocations=1;assert(!folio_cover_draw_image(path,framebuffer,(EpdRect){0,0,684,1216}));fail_allocations=0;
    assert(!s_work_live && !clipped);
}
int main(int argc,char**argv) {
    assert(argc==2);fixture_root=argv[1];
    test_decode("rgba.png",true,192,288);
    test_decode("rgba-large.png",true,192,288);
    test_decode("large.jpg",true,192,288);
    test_decode("palette.png",true,100,100);
    test_decode("transparent.png",true,100,50);
    test_decode("wide.png",true,192,48);
    test_decode("bad-crc.png",false,0,0);
    test_decode("truncated.png",false,0,0);
    test_decode("huge.png",false,0,0);
    test_decode("too-many-pixels.png",false,0,0);
    test_decode("excess-inflate.png",false,0,0);
    test_decode("short-inflate.png",false,0,0);
    test_decode("missing-adler.png",false,0,0);
    test_decode("interlaced.png",false,0,0);
    test_decode("depth16.png",false,0,0);
    test_decode("truncated.jpg",false,0,0);
    test_decode("bad.jpg",false,0,0);
    test_decode("no-scan.jpg",false,0,0);
    test_decode("missing-restart.jpg",false,0,0);
    test_decode("bad-post-sof.jpg",false,0,0);
    size_t size;uint8_t* bytes=read_fixture("transparent.png",&size);assert(bytes);unsigned w,h;
    uint8_t* pixels=cover_decode(bytes,size,&w,&h);assert(pixels);
    for(size_t i=0;i<(size_t)w*h;i++)assert(pixels[i]==255);free(pixels);
    fail_allocations=1;assert(!cover_decode(bytes,size,&w,&h));fail_allocations=0;free(bytes);assert(s_work_live==0);
    assert(!cover_work_alloc(COVER_WORK_MAX+1));
    test_cache_and_layout();
    test_wallpaper();
    // Deterministic corruption of the original PNG and JPEG under ASan/UBSan.
    const char* names[]={"rgba.png","large.jpg"};uint32_t random=0x12345678;
    for(unsigned kind=0;kind<2;++kind) {
        bytes=read_fixture(names[kind],&size);assert(bytes);
        uint8_t* mutated=malloc(size);assert(mutated);
        for(unsigned i=0;i<100;++i){memcpy(mutated,bytes,size);random=random*1664525+1013904223;size_t pos=random%size;mutated[pos]^=1u<<(random%8);pixels=cover_decode(mutated,size,&w,&h);free(pixels);assert(s_work_live==0);}
        free(mutated);free(bytes);
    }
    printf("cover: PNG/JPEG/alpha/limits/corruption/cache/dedup/contain passed; peak decoder heap %zu bytes\n",s_work_peak);
    return 0;
}
