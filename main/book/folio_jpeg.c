#include "folio_jpeg.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
#define jd_prepare folio_scan_jd_prepare
#define jd_decomp folio_scan_jd_decomp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "vendor/tjpgd/tjpgd.c"
#pragma GCC diagnostic pop

typedef struct {
    const uint8_t *input;
    size_t left;
    uint8_t *pixels;
    unsigned width, height, sw, sh;
#ifdef ESP_PLATFORM
    TickType_t last_yield;
#endif
} fit_t;

static size_t input(JDEC *decoder, uint8_t *out, size_t size) {
    fit_t *fit = decoder->device;
    if (size > fit->left) size = fit->left;
    if (out) memcpy(out,fit->input,size);
    fit->input += size;
    fit->left -= size;
    return size;
}

static int output(JDEC *decoder, void *block, JRECT *rect) {
    fit_t *f = decoder->device;
#ifdef ESP_PLATFORM
    if (xTaskGetTickCount() - f->last_yield >= pdMS_TO_TICKS(80)) {
        vTaskDelay(1);
        f->last_yield = xTaskGetTickCount();
    }
#endif
    unsigned stride = rect->right - rect->left + 1;
    unsigned x0 = (rect->left*f->width + f->sw-1)/f->sw;
    unsigned y0 = (rect->top*f->height + f->sh-1)/f->sh;
    for (unsigned y=y0;y<f->height;y++) {
        unsigned sy=y*f->sh/f->height;
        if (sy>rect->bottom) break;
        for (unsigned x=x0;x<f->width;x++) {
            unsigned sx=x*f->sw/f->width;
            if (sx>rect->right) break;
            f->pixels[y*f->width+x]=((uint8_t*)block)[(sy-rect->top)*stride+sx-rect->left];
        }
    }
    return 1;
}

uint8_t *folio_jpeg_fit(const uint8_t *data,size_t size,unsigned max_w,unsigned max_h,unsigned *width,unsigned *height) {
    fit_t f={.input=data,.left=size};
    void *pool=malloc(8192);
    JDEC decoder;
    if (!pool) return NULL;
    if (max_w && max_h && max_w<=1216 && max_h<=1216 && jd_prepare(&decoder,input,pool,8192,&f)==JDR_OK &&
        decoder.width && decoder.height && (uint64_t)decoder.width*decoder.height<=32000000) {
        f.width=max_w; f.height=(unsigned)((uint64_t)decoder.height*f.width/decoder.width);
        if (f.height>max_h) { f.height=max_h; f.width=(unsigned)((uint64_t)decoder.width*f.height/decoder.height); }
        if (!f.width) f.width=1;
        if (!f.height) f.height=1;
        unsigned scale=0;
        while (scale<3 && (unsigned)(decoder.width>>(scale+1))>=f.width && (unsigned)(decoder.height>>(scale+1))>=f.height) ++scale;
        f.sw=decoder.width>>scale; f.sh=decoder.height>>scale;
        f.pixels=malloc(f.width*f.height);
        if (f.pixels && jd_decomp(&decoder,output,scale)!=JDR_OK) { free(f.pixels); f.pixels=NULL; }
        if (f.pixels) { *width=f.width; *height=f.height; }
    }
    free(pool);
    return f.pixels;
}
