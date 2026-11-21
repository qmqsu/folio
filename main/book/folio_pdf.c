#include "folio_pdf.h"
#include "folio_jpeg.h"
#include "pdfio.h"
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
extern bool folio_io_cancelled(void);
#endif

typedef struct {
    unsigned first, last, base;
} mapping_t;

typedef struct {
    const char *name;
    mapping_t *map;
    size_t count, capacity;
    unsigned width;
    bool composite, unicode;
} font_t;

static bool pdf_error(pdfio_file_t *pdf, const char *message, void *data) {
    (void)pdf;
#if defined(CONFIG_FOLIO_USB_DEBUG) && CONFIG_FOLIO_USB_DEBUG
    printf("PDF parser: %.160s\n", message);
#endif
    if (!strncmp(message,"WARNING:",8)) return true;
    *(bool *)data = true;
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool hex_num(const char *token, unsigned *result) {
    size_t len = strlen(token);
    if (*token != '<' || len < 2 || len > 9) return false;
    unsigned n = 0;
    while (*++token) {
        int digit = hex_value(*token);
        if (digit < 0) return false;
        n = (n << 4) | (unsigned)digit;
    }
    *result = n;
    return true;
}

static bool add_range(font_t *font, unsigned first, unsigned last, unsigned base) {
    if (base > 0xffff && (base >> 26) == 0x36 && ((base >> 10) & 63) == 55)
        base = 0x10000 + (((base >> 16) & 1023) << 10) + (base & 1023);
    if (last < first || base > 0x10ffff || last-first > 0x10ffff-base) return false;
    if (font->count == font->capacity) {
        if (font->capacity >= 65536) return false;
        size_t cap = font->capacity ? font->capacity*2 : 32;
        mapping_t *map = realloc(font->map, cap*sizeof(*map));
        if (!map) return false;
        font->map = map;
        font->capacity = cap;
    }
    font->map[font->count++] = (mapping_t){first,last,base};
    return true;
}

static int by_code(const void *a, const void *b) {
    const mapping_t *left = a, *right = b;
    return left->first < right->first ? -1 : left->first > right->first;
}

static bool read_cmap(font_t *font, pdfio_stream_t *stream) {
    char token[256];
    int mode = 0;
    while (pdfioStreamGetToken(stream, token, sizeof(token))) {
        if (!strcmp(token,"beginbfchar")) { mode = 1; continue; }
        if (!strcmp(token,"beginbfrange")) { mode = 2; continue; }
        if (!strncmp(token,"endbf",5)) { mode = 0; continue; }
        if (!mode) continue;
        unsigned first, last, base;
        unsigned width = (strlen(token)-1)/2;
        if (!hex_num(token,&first) || width < 1 || width > 2 ||
            (font->width && font->width != width)) return false;
        font->width = width;
        last = first;
        if (mode == 2 && (!pdfioStreamGetToken(stream,token,sizeof(token)) || !hex_num(token,&last))) return false;
        if (last > 65535 || first > last || !pdfioStreamGetToken(stream,token,sizeof(token))) return false;
        if (!strcmp(token,"[")) {
            for (unsigned code=first; code<=last; ++code)
                if (!pdfioStreamGetToken(stream,token,sizeof(token)) || !hex_num(token,&base) ||
                    !add_range(font,code,code,base)) return false;
            if (!pdfioStreamGetToken(stream,token,sizeof(token)) || strcmp(token,"]")) return false;
        } else if (!hex_num(token,&base) || !add_range(font,first,last,base)) return false;
    }
    if (font->count) qsort(font->map,font->count,sizeof(mapping_t),by_code);
    return mode == 0;
}

static pdfio_dict_t *dictionary(pdfio_dict_t *dict, const char *key) {
    pdfio_dict_t *direct = pdfioDictGetDict(dict,key);
    return direct ? direct : pdfioObjGetDict(pdfioDictGetObj(dict,key));
}

static bool load_font(font_t *font, pdfio_dict_t *dict) {
    const char *type = pdfioDictGetName(dict,"Subtype");
    const char *encoding = pdfioDictGetName(dict,"Encoding");
    font->composite = type && !strcmp(type,"Type0");
    font->unicode = encoding && (strstr(encoding,"UCS2") || strstr(encoding,"UTF16"));
    pdfio_obj_t *obj = pdfioDictGetObj(dict,"ToUnicode");
    if (!obj) return true;
    pdfio_stream_t *stream = pdfioObjOpenStream(obj,true);
    if (!stream) return false;
    bool ok = read_cmap(font,stream);
    pdfioStreamClose(stream);
    return ok;
}

static unsigned decode(const font_t *font, unsigned code) {
    size_t lo = 0, hi = font ? font->count : 0;
    while (lo < hi) {
        const mapping_t *range = font->map + lo+(hi-lo)/2;
        if (code < range->first) hi = (size_t)(range-font->map);
        else if (code > range->last) lo = (size_t)(range-font->map)+1;
        else return range->base + code-range->first;
    }
    if (font && (font->count || (font->composite && !font->unicode))) return 0;
    static const unsigned short win_ansi[32] = {
        0x20ac,0,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0,0x17d,0,
        0,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0,0x17e,0x178};
    if ((!font || !font->composite) && code >= 128 && code < 160) return win_ansi[code-128];
    return code;
}

typedef struct {
    int fd;
    size_t used;
    uint8_t bytes[4096];
} text_output_t;

static bool flush_text(text_output_t *out) {
    size_t written = 0;
    while (written < out->used) {
        ssize_t n = write(out->fd, out->bytes + written, out->used - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += (size_t)n;
    }
    out->used = 0;
    return true;
}

static bool append_text(text_output_t *out, const void *bytes, size_t size) {
    if (size > sizeof(out->bytes) - out->used && !flush_text(out)) return false;
    memcpy(out->bytes + out->used, bytes, size);
    out->used += size;
    return true;
}

static bool write_text(text_output_t *out, const font_t *font, char *token, size_t *total) {
    unsigned width = font && font->width ? font->width : font && font->composite ? 2 : 1;
    bool hex = *token++ == '<';
    size_t len = strlen(token), step = width*(hex ? 2 : 1);
    if (len % step) return false;
    for (size_t i=0; i<len; i+=step) {
        unsigned code = 0;
        for (size_t j=0; j<step; ++j) {
            if (hex) {
                int digit = hex_value(token[i+j]);
                if (digit < 0) return false;
                code = code*16+(unsigned)digit;
            } else code = code*256+(unsigned char)token[i+j];
        }
        unsigned c = decode(font,code);
        if (c == 0 || (c >= 0xd800 && c <= 0xdfff)) return false;
        if (c < 32) continue;
        unsigned char bytes[4];
        int n = c < 128 ? 1 : c < 2048 ? 2 : c < 65536 ? 3 : 4;
        for (int k=n-1;k>0;--k) { bytes[k]=128+(c&63); c >>= 6; }
        bytes[0]=(unsigned char)(c | (n==1 ? 0 : n==2 ? 192 : n==3 ? 224 : 240));
        if (*total >= 32*1024*1024 || !append_text(out,bytes,(size_t)n)) return false;
        *total += n;
    }
    return true;
}

static bool extract_page(pdfio_obj_t *page, text_output_t *out, char *token, size_t *total, const atomic_bool *cancel) {
    pdfio_dict_t *resources = pdfioPageGetDict(page,"Resources");
    if (!resources) resources = pdfioObjGetDict(pdfioPageGetObj(page,"Resources"));
    pdfio_dict_t *fonts = dictionary(resources,"Font");
    size_t n = pdfioDictGetNumPairs(fonts);
    if (n > 256) return false;
    font_t *list = calloc(n+1,sizeof(*list));
    if (!list) return false;
    bool ok = true;
    for (size_t i=0; i<n && ok; ++i) {
        list[i].name = pdfioDictGetKey(fonts,i);
        ok = load_font(list+i,dictionary(fonts,list[i].name));
    }
    bool text = false, line = false;
    font_t *active = NULL, *named = NULL;
    for (size_t i=0;ok && i<pdfioPageGetNumStreams(page);++i) {
        pdfio_stream_t *stream = pdfioPageOpenStream(page,i,true);
        if (!stream) { ok=false; break; }
        while (ok && !atomic_load(cancel) && pdfioStreamGetToken(stream,token,32768)) {
            if (!strcmp(token,"BT")) text=true;
            else if (text && *token=='/') {
                named=NULL;
                for (size_t j=0;j<n;++j) if (!strcmp(list[j].name,token+1)) { named=list+j; break; }
            } else if (text && !strcmp(token,"Tf")) active=named;
            else if (text && (*token=='(' || (*token=='<' && token[1]!='<'))) {
                ok=write_text(out,active,token,total);
                line=true;
            } else if (text && (!strcmp(token,"ET") || !strcmp(token,"Td") || !strcmp(token,"TD") ||
                                !strcmp(token,"Tm") || !strcmp(token,"T*"))) {
                if (line) ok=append_text(out,"\n",1);
                line=false;
                if (!strcmp(token,"ET")) text=false;
            }
        }
        pdfioStreamClose(stream);
    }
    if (line && !append_text(out,"\n",1)) ok=false;
    for (size_t i=0;i<n;++i) free(list[i].map);
    free(list);
    return ok && !atomic_load(cancel);
}

static bool extract(const char *path, FILE *out, const atomic_bool *cancel) {
    bool error=false;
    pdfio_file_t *pdf=pdfioFileOpen(path,NULL,NULL,pdf_error,&error);
    if (!pdf) return false;
    text_output_t *writer=calloc(1,sizeof(*writer));
    if (writer) writer->fd=fileno(out);
    char *token=malloc(32768);
    size_t pages=pdfioFileGetNumPages(pdf), total=0;
    bool ok=writer && token && pages && pages<=8192;
    for (size_t i=0;ok && !error && i<pages;++i) {
#ifdef ESP_PLATFORM
        if (atomic_load(cancel)) { ok=false; break; }
        vTaskDelay(1);
#endif
        ok=extract_page(pdfioFileGetPage(pdf,i),writer,token,&total,cancel);
    }
    free(token);
    pdfioFileClose(pdf);
    ok = ok && !error && total && flush_text(writer);
    free(writer);
    return ok;
}


static pdfio_obj_t *scan_image(pdfio_obj_t *page) {
    pdfio_dict_t *resources=pdfioPageGetDict(page,"Resources");
    if (!resources) resources=pdfioObjGetDict(pdfioPageGetObj(page,"Resources"));
    for (unsigned depth=0;resources && depth<8;++depth) {
        pdfio_dict_t *objects=dictionary(resources,"XObject");
        if (pdfioDictGetNumPairs(dictionary(resources,"Font")) || pdfioDictGetNumPairs(objects)!=1) return NULL;
        pdfio_obj_t *obj=pdfioDictGetObj(objects,pdfioDictGetKey(objects,0));
        pdfio_dict_t *dict=pdfioObjGetDict(obj);
        const char *type=pdfioDictGetName(dict,"Subtype");
        if (!type) return NULL;
        if (!strcmp(type,"Image")) {
            const char *filter=pdfioDictGetName(dict,"Filter");
            return filter && !strcmp(filter,"DCTDecode") ? obj : NULL;
        }
        if (strcmp(type,"Form")) return NULL;
        resources=dictionary(dict,"Resources");
    }
    return NULL;
}

typedef struct {
    const char *path;
    FILE *output;
    unsigned page, max_w, max_h;
    folio_pdf_scan_t *scan;
    atomic_bool cancel;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t done;
#endif
    bool result;
} pdf_job_t;

static bool scan_page(pdf_job_t *job) {
    bool error=false;
    pdfio_file_t *pdf=pdfioFileOpen(job->path,NULL,NULL,pdf_error,&error);
    if (!pdf) return false;
    size_t count=pdfioFileGetNumPages(pdf);
    pdfio_obj_t *obj=count<=8192 && job->page<count ? scan_image(pdfioFileGetPage(pdf,job->page)) : NULL;
    // Only select scan mode for a document made of scanned pages. A cover
    // image preceding ordinary text must not disable text reflow.
    if (obj && !job->scan->count) {
        for (size_t i=1;i<count && obj;i++) {
            if (atomic_load(&job->cancel) || !scan_image(pdfioFileGetPage(pdf,i))) obj=NULL;
        }
    }
    size_t size=obj ? pdfioObjGetLength(obj) : 0;
    pdfio_stream_t *stream=obj ? pdfioObjOpenStream(obj,false) : NULL;
    uint8_t *jpeg=stream && size && size<=4u*1024*1024 ? malloc(size) : NULL;
    size_t used=0;
    while (jpeg && used<size && !atomic_load(&job->cancel)) {
        size_t chunk=size-used;
        if (chunk>16384) chunk=16384;
        ssize_t n=pdfioStreamRead(stream,jpeg+used,chunk);
        if (n<=0) break;
        used+=(size_t)n;
    }
#if defined(CONFIG_FOLIO_USB_DEBUG) && CONFIG_FOLIO_USB_DEBUG
    printf("PDF scan: pages=%u index=%u object=%d read=%u/%u error=%d\n", (unsigned)count, job->page, obj!=NULL, (unsigned)used,(unsigned)size,error);
#endif
    if (stream) pdfioStreamClose(stream);
    pdfioFileClose(pdf);
    if (!error && jpeg && used==size)
        job->scan->pixels=folio_jpeg_fit(jpeg,size,job->max_w,job->max_h,&job->scan->width,&job->scan->height);
    free(jpeg);
    if (!job->scan->pixels) return false;
    job->scan->count=(unsigned)count;
    return true;
}
#ifdef ESP_PLATFORM
static void worker(void *arg) {
    pdf_job_t *job=arg;
    job->result=job->scan ? scan_page(job) : extract(job->path,job->output,&job->cancel);
    xSemaphoreGive(job->done);
    vTaskDeleteWithCaps(NULL);
}
#endif

static bool run_job(pdf_job_t *job) {
#ifdef ESP_PLATFORM
    job->done=xSemaphoreCreateBinary();
    if (!job->done) return false;
    if (xTaskCreateWithCaps(worker,"pdf",384*1024,job,2,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS)
        while (!xSemaphoreTake(job->done,pdMS_TO_TICKS(100))) {
            if (folio_io_cancelled()) atomic_store(&job->cancel,true);
        }
    vSemaphoreDelete(job->done);
    return job->result;
#else
    return job->scan ? scan_page(job) : extract(job->path,job->output,&job->cancel);
#endif
}

bool folio_pdf_extract(const char *path, FILE *output) {
    pdf_job_t job={.path=path,.output=output};
    return run_job(&job);
}

bool folio_pdf_scan_page(const char *path,unsigned page,unsigned max_w,unsigned max_h,folio_pdf_scan_t *result) {
    pdf_job_t job={.path=path,.page=page,.max_w=max_w,.max_h=max_h,.scan=result};
    return run_job(&job);
}
