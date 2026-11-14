#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
static unsigned cancel_after, cancel_calls, allocation_calls, fail_at;
static size_t allocated, peak_allocated;
static uint64_t fake_time, fake_step;
static bool timeout_in_legacy, change_source;
static char change_path[1400];
static uint64_t test_now(void);
static int test_fstat(int fd, struct stat* value);
static int test_stat(const char* path, struct stat* value);
static unsigned named_stat_calls, fd_stat_calls, alter_named_at, alter_fd_at;
enum { ALTER_NONE, ALTER_SIZE, ALTER_MTIME, ALTER_INODE, ALTER_DEVICE, ALTER_MODE, ALTER_MTIME_NS };
static unsigned alteration;
#define fstat(fd, value) test_fstat(fd, value)
#define stat(path, value) test_stat(path, value)
#define FOLIO_WALLPAPER_NOW_MS test_now
#include "../../main/book/folio_wallpaper.c"
#undef fstat
#undef stat

static void alter_stat(struct stat* value) {
    switch (alteration) {
        case ALTER_SIZE: ++value->st_size; break;
        case ALTER_MTIME: value->st_mtime += 2; break;
        case ALTER_INODE: ++value->st_ino; break;
        case ALTER_DEVICE: ++value->st_dev; break;
        case ALTER_MODE: value->st_mode = S_IFDIR | 0777; break;
        case ALTER_MTIME_NS:
#if defined(__APPLE__) && defined(st_mtime)
            value->st_mtimespec.tv_nsec = (value->st_mtimespec.tv_nsec + 1) % 1000000000L;
#elif defined(__APPLE__)
            value->st_mtimensec = (value->st_mtimensec + 1) % 1000000000L;
#else
            value->st_mtim.tv_nsec = (value->st_mtim.tv_nsec + 1) % 1000000000L;
#endif
            break;
        default: break;
    }
}
static int test_fstat(int fd, struct stat* value) {
    int result = fstat(fd, value);
    if (result) return result;
#ifdef FOLIO_WALLPAPER_TEST_FATFS
    /* Match IDF vfs_fat_fstat(): every field zero except size/mode/blksize. */
    off_t bytes = value->st_size; memset(value, 0, sizeof(*value));
    value->st_size = bytes; value->st_mode = S_IFREG | 0777; value->st_blksize = 4096;
#endif
    if (++fd_stat_calls == alter_fd_at) alter_stat(value);
    return 0;
}
static int test_stat(const char* path, struct stat* value) {
    int result = stat(path, value);
    if (result) return result;
#ifdef FOLIO_WALLPAPER_TEST_FATFS
    /* Match IDF pathname stat(): FAT supplies size/mode and real mtime, not
     * descriptor identity or ctime. Keep a nonzero timestamp in this fixture. */
    off_t bytes = value->st_size; time_t modified = value->st_mtime;
    mode_t type = value->st_mode & S_IFMT; memset(value, 0, sizeof(*value));
    value->st_size = bytes; value->st_mode = type | 0777; value->st_mtime = modified;
#endif
    if (++named_stat_calls == alter_named_at) alter_stat(value);
    return 0;
}

static uint64_t test_now(void) {
    fake_time += timeout_in_legacy && s_wall_context && s_wall_context->legacy && s_wall_context->image.pixels ? 5000 : fake_step;
    return fake_time;
}
typedef union { size_t size; max_align_t align; } test_header_t;
bool folio_io_cancelled(void) {
    if (change_source && s_wall_context && s_wall_context->image.pixels) {
        FILE* f=fopen(change_path,"ab");assert(f);assert(fputc('x',f)!=EOF);assert(!fclose(f));change_source=false;
    }
    return cancel_after && ++cancel_calls >= cancel_after;
}
void* heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) || caps == MALLOC_CAP_8BIT);
    ++allocation_calls;
    if (fail_at && allocation_calls == fail_at) return NULL;
    /* Use malloc directly: ASan/LSan account for all code, including legacy. */
    (void)allocated; (void)peak_allocated;
    return malloc(bytes);
}
int epd_rotated_display_width(void) { return 684; }
int epd_rotated_display_height(void) { return 1216; }
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* pixels) {
    assert(x >= 0 && x < 684 && y >= 0 && y < 1216 && color % 17 == 0);
    pixels[(size_t)y * 684 + x] = color;
}
bool folio_epub_cover(const char* p, uint8_t** d, size_t* s, char* e, size_t n) {
    (void)p; (void)e; (void)n; *d = NULL; *s = 0; return false;
}
static char root[1024];
static void path_for(const char* name, char* path, size_t cap) { snprintf(path, cap, "%s/%s", root, name); }
static unsigned get_pixel(const folio_wallpaper_image_t* image, unsigned x, unsigned y) {
    unsigned p = image->pixels[(size_t)y * image->stride + x / 2];
    return x & 1 ? p & 15 : p >> 4;
}
static bool decode(const char* name, unsigned w, unsigned h, folio_wallpaper_image_t* image, char error[256]) {
    char path[1400]; path_for(name, path, sizeof(path));
    bool okay = folio_wallpaper_decode_file(path, w, h, image, error, 256);
    assert(!s_wall_context && !s_wall_last_live);
    assert(s_wall_last_peak <= FOLIO_WALLPAPER_WORK_MAX);
    if (!okay) assert(!image->pixels && !image->width && !image->height && !image->stride);
    return okay;
}
static void check(const char* name, bool expected, const char* kind) {
    folio_wallpaper_image_t image = {0}; char error[256];
    bool okay = decode(name, 620, 1040, &image, error);
    if (okay != expected) fprintf(stderr, "%s expected %d got %d: %s\n", name, expected, okay, error);
    assert(okay == expected);
    if (kind && !okay) { if (!strstr(error, kind)) fprintf(stderr, "%s wrong error: %s\n", name, error); assert(strstr(error, kind)); }
    printf("wallpaper %s: %s peak=%zu legacy=%zu %s\n", name, okay ? "OK" : "rejected", s_wall_last_peak, s_wall_last_legacy_peak, error);
    if (okay) assert(image.width == 620 && image.height == 1040 && image.stride == 310);
    folio_wallpaper_image_free(&image);
}
static void directions(void) {
    for (unsigned orientation = 1; orientation <= 8; ++orientation) {
        char name[64], error[256]; snprintf(name, sizeof(name), "exif-%u.jpg", orientation);
        unsigned w = orientation >= 5 ? 80 : 120, h = orientation >= 5 ? 120 : 80;
        folio_wallpaper_image_t image = {0}; assert(decode(name, w, h, &image, error));
        static const unsigned corners[8][4] = {{0,5,10,15},{5,0,15,10},{15,10,5,0},{10,15,0,5},{0,10,5,15},{10,0,15,5},{15,5,10,0},{5,15,0,10}};
        unsigned xs[] = {10,w-11,10,w-11}, ys[] = {10,10,h-11,h-11};
        for (unsigned i = 0; i < 4; ++i) {
            unsigned actual = get_pixel(&image, xs[i], ys[i]);
            if (actual != corners[orientation-1][i]) fprintf(stderr, "orientation %u corner %u = %u expected %u\n", orientation,i,actual,corners[orientation-1][i]);
            assert(actual == corners[orientation-1][i]);
        }
        folio_wallpaper_image_free(&image);
    }
    folio_wallpaper_image_t big={0}, little={0};char error[256];
    assert(decode("exif-6.jpg",80,120,&big,error));assert(decode("exif-little.jpg",80,120,&little,error));
    assert(big.stride==little.stride && !memcmp(big.pixels,little.pixels,big.stride*big.height));
    folio_wallpaper_image_free(&big);folio_wallpaper_image_free(&little);
}
static void quality_and_commit(void) {
    folio_wallpaper_image_t image = {0}; char error[256];
    assert(decode("stripes.png", 684, 1216, &image, error));
    for (unsigned y = 0; y < 1216; ++y) for (unsigned x = 0; x < 684; ++x) assert(get_pixel(&image,x,y) == (x&1 ? 15 : 0));
    uint8_t* fb = malloc(684*1216); assert(fb); memset(fb,33,684*1216);
    assert(folio_wallpaper_draw_image(&image, fb, (EpdRect){10,20,100,200}));
    assert(fb[0] == 33 && fb[19*684+10] == 33 && fb[220*684+10] == 33);
    memset(fb,33,684*1216);
    assert(!folio_wallpaper_draw_image(&image, fb, (EpdRect){0,0,685,1216}));
    cancel_after = 1; cancel_calls = 0;
    assert(!folio_wallpaper_draw_image(&image, fb, (EpdRect){0,0,684,1216})); cancel_after = 0;
    for (size_t i=0;i<684*1216;++i) assert(fb[i]==33);
    folio_wallpaper_image_free(&image); free(fb);
    assert(decode("stripes.png", 342,608,&image,error));
    for (unsigned y=0;y<608;++y) for (unsigned x=0;x<342;++x) assert(get_pixel(&image,x,y)==7 || get_pixel(&image,x,y)==8);
    folio_wallpaper_image_free(&image);
    assert(decode("odd.png", 17,19,&image,error)); assert(image.stride==9);
    for(unsigned y=0;y<19;++y) { assert((image.pixels[y*image.stride+8]&15)==15); assert(get_pixel(&image,16,y)==15); }
    folio_wallpaper_image_free(&image);
}
static void errors(void) {
    const char* files[]={"small.png","small.jpg","progressive.jpg"};
    for(unsigned file=0;file<3;++file) {
        for(unsigned i=1;i<=12;++i) {
            folio_wallpaper_image_t image={0}; char error[256];
            fail_at=i; allocation_calls=0;
            (void)decode(files[file],620,1040,&image,error);
            folio_wallpaper_image_free(&image);
        }
        fail_at=0;
        for(unsigned i=1;i<=40;++i) {
            folio_wallpaper_image_t image={0}; char error[256];
            cancel_after=i; cancel_calls=0;
            bool okay=decode(files[file],620,1040,&image,error);
            if(!okay)assert(strstr(error,"cancelled"));
            folio_wallpaper_image_free(&image);
        }
        cancel_after=0;
    }
    fake_step=5000; check("small.png",false,"timeout");fake_step=0;
    timeout_in_legacy=true;check("progressive.jpg",false,"timeout");timeout_in_legacy=false;
    check("small.jpg",true,NULL); /* bridge timeout callback was reset */
    char source[1400];path_for("small.png",source,sizeof(source));path_for("changing.png",change_path,sizeof(change_path));
    FILE* from=fopen(source,"rb");FILE* to=fopen(change_path,"wb");assert(from&&to);
    int b;while((b=fgetc(from))!=EOF)assert(fputc(b,to)!=EOF);assert(!fclose(from));assert(!fclose(to));
    change_source=true;check("changing.png",false,"io");assert(!change_source);
}
static void stat_contract_tests(void) {
    char path[1400]; path_for("small.jpg", path, sizeof(path));
    struct stat named, opened; FILE* file = fopen(path, "rb"); assert(file);
    assert(!test_stat(path, &named) && !test_fstat(fileno(file), &opened)); assert(!fclose(file));
#ifdef FOLIO_WALLPAPER_TEST_FATFS
    assert(named.st_mtime != 0 && opened.st_mtime == 0 && !opened.st_ino && !opened.st_dev);
    assert(!wall_same_path_metadata(&named, &opened));
#endif
    /* A successful complete real decode must tolerate FAT's asymmetric APIs. */
    check("small.jpg",true,NULL); check("rgba.png",true,NULL);
    const unsigned named_changes[] = {ALTER_SIZE, ALTER_MTIME, ALTER_MODE,
#if !defined(FOLIO_WALLPAPER_TEST_FATFS)
        ALTER_INODE, ALTER_DEVICE, ALTER_MTIME_NS,
#endif
    };
    for(unsigned i=0;i<sizeof(named_changes)/sizeof(*named_changes);++i) {
        alteration=named_changes[i]; named_stat_calls=fd_stat_calls=0; alter_named_at=2;
        check("small.jpg",false,"io"); alter_named_at=0;
    }
    const unsigned fd_changes[] = {ALTER_SIZE, ALTER_MODE,
#if !defined(FOLIO_WALLPAPER_TEST_FATFS)
        ALTER_MTIME, ALTER_INODE, ALTER_DEVICE, ALTER_MTIME_NS,
#endif
    };
    for(unsigned i=0;i<sizeof(fd_changes)/sizeof(*fd_changes);++i) {
        alteration=fd_changes[i]; named_stat_calls=fd_stat_calls=0; alter_fd_at=2;
        check("small.jpg",false,"io"); alter_fd_at=0;
    }
    /* Reject pathname/open-FIL size mismatch before allocating a canvas. */
    alteration=ALTER_SIZE; named_stat_calls=fd_stat_calls=0; alter_fd_at=1;
    check("small.jpg",false,"io");alter_fd_at=alteration=0;
#ifdef FOLIO_WALLPAPER_TEST_FATFS
    puts("wallpaper FAT API contract: real decoder accepts fstat zero mtime; path mtime/size and descriptor size/mode changes still reject");
#else
    puts("wallpaper POSIX API contract: descriptor/path inode, device, mtime, nanoseconds, size and mode protections retained");
#endif
}
static void mutation_tests(void) {
    uint32_t random=0x831C0DEu;
    const char* names[]={"small.jpg","small.png","progressive.jpg"};
    for(unsigned kind=0;kind<3;++kind) {
        char path[1400];path_for(names[kind],path,sizeof(path));FILE* file=fopen(path,"rb");assert(file);
        assert(!fseek(file,0,SEEK_END));long n=ftell(file);assert(n>0 && n<65536);rewind(file);
        uint8_t* bytes=malloc((size_t)n);uint8_t* copy=malloc((size_t)n);assert(bytes&&copy);
        assert(fread(bytes,1,(size_t)n,file)==(size_t)n);assert(!fclose(file));
        path_for("mutant.bin",path,sizeof(path));
        for(unsigned iteration=0;iteration<1500;++iteration) {
            memcpy(copy,bytes,(size_t)n);
            for(unsigned j=0;j<1+iteration%4;++j) {random=random*1664525u+1013904223u;size_t pos=random%(size_t)n;random=random*1664525u+1013904223u;copy[pos]^=(uint8_t)(1u<<(random%8));}
            file=fopen(path,"wb");assert(file);assert(fwrite(copy,1,(size_t)n,file)==(size_t)n);assert(!fclose(file));
            folio_wallpaper_image_t image={0};char error[256];
            (void)decode("mutant.bin",64,96,&image,error);folio_wallpaper_image_free(&image);
        }
        free(bytes);free(copy);
    }
    puts("wallpaper: 4500 deterministic malformed-input mutations passed");
}
int main(int argc,char**argv) {
    assert(argc==2); snprintf(root,sizeof(root),"%s",argv[1]);
    const char* good[]={"small.png","small.jpg","large.png","large.jpg","large-gray.jpg","max-bytes.png","max-bytes.jpg","odd.jpg","tiny.jpg","gray.jpg","progressive.jpg","cmyk.jpg","rgba.png","palette.png","restart.jpg","split-idat.png","exif-little.jpg"};
    for(unsigned i=0;i<sizeof(good)/sizeof(*good);++i)check(good[i],true,NULL);
    const char* bad[]={"truncated.png","bad-crc.png","excess.png","short.png","truncated.jpg","missing-eoi.jpg","extra-entropy.jpg","no-scan.jpg","two-sof.jpg","bad-exif.jpg","bad-dht.jpg","bad-restart.jpg","tail.png"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)check(bad[i],false,NULL);
    check("too-large.png",false,"size"); check("too-many-pixels.png",false,"pixels");check("too-wide.png",false,"dimensions");
    check("interlaced.png",false,"format");check("depth16.png",false,"format");check("animation.png",false,"format");
    check("large-progressive.jpg",false,"format");check("picture.bmp",false,"format");
    check("max-pixels.png",true,NULL);check("max-width.png",true,NULL);
    const char* extremes[]={"wide-rgba.png","wide.jpg","large-gray.jpg","large-gray.jpg"};
    for(unsigned i=0;i<4;++i) {
        folio_wallpaper_image_t image={0};char error[256];
        unsigned w=i<2?684:i==2?400:64,h=i<2?1216:i==2?600:96;
        assert(decode(extremes[i],w,h,&image,error));
        printf("wallpaper full/scale stress %s %ux%u peak=%zu\n",extremes[i],w,h,s_wall_last_peak);
        folio_wallpaper_image_free(&image);
    }
    directions();quality_and_commit();errors();stat_contract_tests();mutation_tests();
    puts("wallpaper: real large PNG/JPEG, 32MiB boundaries, EXIF1-8, area filtering, failure cleanup, cancellation and timeout passed");
    return 0;
}
