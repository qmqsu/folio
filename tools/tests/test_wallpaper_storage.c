/* Real files + the actual storage module; only isolated temporary roots. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void* tracked_malloc(size_t);
static void tracked_free(void*);
static size_t test_fread(void*,size_t,size_t,FILE*);
static size_t test_fwrite(const void*,size_t,size_t,FILE*);
static int test_fflush(FILE*);
static int test_fsync(int);
static int test_fclose(FILE*);
static FILE* test_fdopen(int,const char*);
#define fdopen test_fdopen
#define malloc tracked_malloc
#define free tracked_free
#define fread test_fread
#define fwrite test_fwrite
#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
/* Compile a strong hook separately, just like the actual application linkage. */
#define folio_storage_cancelled wallpaper_default_cancelled
#include "../../main/folio_storage.c"
#undef folio_storage_cancelled
#undef fdopen
#undef malloc
#undef free
#undef fread
#undef fwrite
#undef fflush
#undef fsync
#undef fclose

/* The include's hook is renamed; calls go through an explicit strong IO hook. */
extern unsigned wallpaper_cancel_at,wallpaper_cancel_calls;
extern unsigned wallpaper_crash_at;
unsigned wallpaper_cancel_at,wallpaper_cancel_calls,wallpaper_crash_at;

enum { IO_NONE,IO_READ,IO_WRITE,IO_FLUSH,IO_SYNC,IO_CLOSE,IO_KINDS };
static unsigned io_kind,io_at,io_calls[IO_KINDS],commit_writes,crash_kind,crash_at;
static size_t read_bytes,allocation_calls,allocation_fail_at=SIZE_MAX,live_bytes,peak_bytes,live_count;
static struct { void* p;size_t size; } allocations[32];
static unsigned checks,live_files;
static char error[256];
static const char* source_a=FOLIO_PICTURE_ROOT "/a.png";
static const char* source_b=FOLIO_PICTURE_ROOT "/b.jpg";
static uint8_t small_a[16],small_b[16];
#define CHECK(test) do {++checks;if(!(test)){fprintf(stderr,"line %d: %s (error=%s)\n",__LINE__,#test,error);abort();}} while(0)

static void* tracked_malloc(size_t size) {
    ++allocation_calls;
    if(allocation_calls>=allocation_fail_at)return NULL;
    void* p=malloc(size);if(!p)return NULL;
    for(unsigned i=0;i<32;++i)if(!allocations[i].p) {
        allocations[i].p=p;allocations[i].size=size;live_bytes+=size;++live_count;
        if(live_bytes>peak_bytes)peak_bytes=live_bytes;return p;
    }
    abort();
}
static void tracked_free(void* p) {
    if(!p)return;
    for(unsigned i=0;i<32;++i)if(allocations[i].p==p) {
        live_bytes-=allocations[i].size;--live_count;allocations[i].p=NULL;free(p);return;
    }
    abort();
}
static bool io_failure(unsigned kind) {
    unsigned call=++io_calls[kind];
    if(crash_kind==kind && crash_at==call)_Exit(76);
    return call==io_at && io_kind==kind;
}
static size_t test_fread(void* p,size_t size,size_t count,FILE* f) {
    if(io_failure(IO_READ)){errno=EIO;return 0;}
    size_t got=fread(p,size,count,f);read_bytes+=got*size;return got;
}
static size_t test_fwrite(const void* p,size_t size,size_t count,FILE* f) {
    if(size*count==sizeof(wallpaper_record_t))++commit_writes;
    if(io_failure(IO_WRITE)){errno=EIO;return fwrite(p,size,count/2,f);}
    return fwrite(p,size,count,f);
}
static int test_fflush(FILE* f) {if(io_failure(IO_FLUSH)){errno=EIO;return EOF;}return fflush(f);}
static int test_fsync(int fd) {if(io_failure(IO_SYNC)){errno=EIO;return -1;}return fsync(fd);}
static FILE* test_fdopen(int fd,const char* mode) {FILE* f=fdopen(fd,mode);if(f)++live_files;return f;}
static int test_fclose(FILE* f) {
    CHECK(live_files);--live_files;int result=fclose(f);
    if(io_failure(IO_CLOSE)){errno=EIO;return EOF;}return result;
}
static void faults_reset(void) {
    io_kind=io_at=commit_writes=crash_kind=crash_at=0;memset(io_calls,0,sizeof(io_calls));
    wallpaper_cancel_at=wallpaper_cancel_calls=wallpaper_crash_at=0;
    allocation_calls=read_bytes=0;allocation_fail_at=SIZE_MAX;error[0]=0;
    CHECK(!live_bytes && !live_count && !live_files);
}
static void create_source(const char* path,size_t bytes) {
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);
    CHECK(ftruncate(fd,(off_t)bytes)==0 && close(fd)==0);
}
static void remove_slots(void) {
    CHECK(strstr(FOLIO_SD_ROOT,"folio-wallpaper-storage-") && strcmp(FOLIO_SD_ROOT,"/sdcard"));
    faults_reset();
    for(unsigned i=0;i<2;++i) {
        char path[256];snprintf(path,sizeof(path),FOLIO_SD_ROOT "/.folio/wallpaper.%c",'a'+i);
        CHECK(unlink(path)==0 || errno==ENOENT);wallpaper_cache_path(i,path);
        CHECK(unlink(path)==0 || errno==ENOENT);
    }
}
static unsigned active_record(wallpaper_record_t* record) {
    wallpaper_record_t all[2];bool valid[2]={wallpaper_record_read(0,&all[0]),wallpaper_record_read(1,&all[1])};
    unsigned slot=wallpaper_newest(all,valid);CHECK(valid[slot]);if(record)*record=all[slot];return slot;
}
static void expect_path(const char* expected) {
    char path[256];CHECK(folio_storage_wallpaper_load(path,sizeof(path)) && !strcmp(path,expected));
}
static void expect_pixels(const uint8_t* expected,unsigned w,unsigned h) {
    uint8_t* pixels=NULL;unsigned width=0,height=0;
    CHECK(folio_storage_wallpaper_cache_load(&pixels,&width,&height));
    CHECK(width==w && height==h && !memcmp(pixels,expected,((w+1u)/2u)*h));tracked_free(pixels);
    CHECK(!live_count && !live_bytes);
}
static void expect_no_cache(void) {
    uint8_t* pixels=(void*)1;unsigned w=1,h=1;
    CHECK(!folio_storage_wallpaper_cache_load(&pixels,&w,&h));CHECK(!pixels && !w && !h && !live_count);
}
static void initial_a(void) {
    remove_slots();CHECK(folio_storage_wallpaper_save_prepared(source_a,small_a,8,4,error,sizeof(error)));
    expect_path(source_a);expect_pixels(small_a,8,4);faults_reset();
}
static void read_cache(unsigned slot,wallpaper_cache_t* header,uint8_t** pixels) {
    char path[256];wallpaper_cache_path(slot,path);FILE* f=fopen(path,"rb");CHECK(f);
    CHECK(fread(header,1,sizeof(*header),f)==sizeof(*header));*pixels=malloc(header->bytes);CHECK(*pixels);
    CHECK(fread(*pixels,1,header->bytes,f)==header->bytes && fclose(f)==0);
}
static void write_cache(unsigned slot,const wallpaper_cache_t* header,const uint8_t* pixels,size_t count) {
    char path[256];wallpaper_cache_path(slot,path);FILE* f=fopen(path,"wb");CHECK(f);
    CHECK(fwrite(header,1,sizeof(*header),f)==sizeof(*header));
    CHECK(fwrite(pixels,1,count,f)==count && fclose(f)==0);
}
static void test_bounds(void) {
    const size_t sizes[]={2u*1024u*1024u-1,2u*1024u*1024u,2u*1024u*1024u+1,
        FOLIO_WALLPAPER_INPUT_MAX-1,FOLIO_WALLPAPER_INPUT_MAX,FOLIO_WALLPAPER_INPUT_MAX+1u};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        initial_a();create_source(source_b,sizes[i]);
        bool expected=sizes[i]<=FOLIO_WALLPAPER_INPUT_MAX;
        CHECK(folio_storage_wallpaper_save(source_b,error,sizeof(error))==expected);
        CHECK(folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error))==expected);
        if(expected) {
            expect_path(source_b);expect_pixels(small_b,8,4);
            char path[256];read_bytes=0;
            CHECK(folio_storage_wallpaper_load_legacy(path,sizeof(path))==(sizes[i]<=2u*1024u*1024u));
            CHECK(read_bytes<2048); /* No image content read for ordinary records. */
        } else {expect_path(source_a);expect_pixels(small_a,8,4);}
        folio_storage_entry_t entries[8];size_t count,total;
        CHECK(folio_storage_wallpapers(0,entries,8,&count,&total,error,sizeof(error)));
        bool listed=false;for(size_t n=0;n<count;++n)if(!strcmp(entries[n].path,source_b))listed=true;
        CHECK(listed==expected);
    }
    create_source(source_b,64);initial_a();
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,NULL,8,4,error,sizeof(error)));
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,small_b,0,4,error,sizeof(error)));
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,small_b,685,4,error,sizeof(error)));
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,small_b,8,1217,error,sizeof(error)));
    CHECK(!folio_storage_wallpaper_save_prepared(FOLIO_PICTURE_ROOT "/../a.png",small_b,8,4,error,sizeof(error)));
    expect_path(source_a);expect_pixels(small_a,8,4);
    uint8_t odd[]={0x01,0x2f,0x34,0x5f};
    CHECK(folio_storage_wallpaper_save_prepared(source_b,odd,3,2,error,sizeof(error)));expect_pixels(odd,3,2);
    puts("wallpaper storage: 2MiB +/-1, 32MiB +/-1, listing/save/load/legacy and odd stride bounds");
}
static void test_full_cache(void) {
    initial_a();create_source(source_b,FOLIO_WALLPAPER_INPUT_MAX);
    size_t bytes=684u*1216u/2u;uint8_t* pixels=malloc(bytes);CHECK(pixels);
    for(size_t i=0;i<bytes;++i)pixels[i]=(uint8_t)(i*37u);
    CHECK(folio_storage_wallpaper_save_prepared(source_b,pixels,684,1216,error,sizeof(error)));
    read_bytes=0;peak_bytes=0;expect_pixels(pixels,684,1216);
    CHECK(read_bytes<bytes+2048 && peak_bytes==bytes);
    allocation_fail_at=1;allocation_calls=0;expect_no_cache();faults_reset();expect_pixels(pixels,684,1216);
    unsigned load_points=wallpaper_cancel_calls;
    for(unsigned point=1;point<=load_points;++point) {
        faults_reset();wallpaper_cancel_at=point;expect_no_cache();wallpaper_cancel_at=0;
    }
    initial_a();CHECK(folio_storage_wallpaper_save_prepared(source_b,pixels,684,1216,error,sizeof(error)));
    unsigned save_points=wallpaper_cancel_calls;
    for(unsigned point=1;point<=save_points;++point) {
        initial_a();wallpaper_cancel_at=point;
        CHECK(!folio_storage_wallpaper_save_prepared(source_b,pixels,684,1216,error,sizeof(error)));
        CHECK(!commit_writes);wallpaper_cancel_at=0;expect_path(source_a);expect_pixels(small_a,8,4);
    }
    printf("wallpaper storage: full canvas cancellation at %u save / %u load points; zero allocations/handles retained\n",save_points,load_points);
    free(pixels);create_source(source_b,64);
    printf("wallpaper storage: full cache 415872 payload bytes; peak allocation %zu; no large-source reads\n",peak_bytes);
}
static void test_corruption(void) {
    initial_a();unsigned slot=active_record(NULL);wallpaper_cache_t original;uint8_t* bytes;
    read_cache(slot,&original,&bytes);
    for(unsigned test=0;test<18;++test) {
        wallpaper_cache_t bad=original;size_t count=original.bytes;
        switch(test) {
            case 0:bad.magic^=1;break;case 1:++bad.version;break;case 2:++bad.layout;break;
            case 3:bad.slot=1-slot;break;case 4:++bad.generation;break;
            case 5:bad.source=WALLPAPER_LEGACY;break;case 6:strcpy(bad.name,"b.jpg");break;
            case 7:bad.width=0;break;case 8:bad.width=685;break;case 9:bad.height=1217;break;
            case 10:++bad.bytes;break;case 11:++bad.stride;break;case 12:bad.pixels_crc^=1;break;
            case 13:--count;break;case 14:bad.header_crc^=1;break;
            case 15:++bad.source_bytes;break;case 16:++bad.source_modified[0];break;
            case 17:++bad.source_modified_ns;break;
        }
        if(test!=14)bad.header_crc=wallpaper_crc32(0,&bad,offsetof(wallpaper_cache_t,header_crc));
        write_cache(slot,&bad,bytes,count);expect_no_cache();
    }
    write_cache(slot,&original,bytes,original.bytes);expect_pixels(small_a,8,4);
    char cache_path[256];wallpaper_cache_path(slot,cache_path);
    FILE* f=fopen(cache_path,"ab");CHECK(f && fputc(0,f)!=EOF && fclose(f)==0);expect_no_cache();
    write_cache(slot,&original,bytes,original.bytes);
    CHECK(unlink(source_a)==0);expect_no_cache();create_source(source_a,64);expect_no_cache();
    CHECK(folio_storage_wallpaper_save_prepared(source_a,small_a,8,4,error,sizeof(error)));expect_pixels(small_a,8,4);
    create_source(source_a,65);expect_no_cache();create_source(source_a,64);
    CHECK(folio_storage_wallpaper_save_prepared(source_a,small_a,8,4,error,sizeof(error)));
    struct stat st;CHECK(stat(source_a,&st)==0);
    struct timespec times[2]={{.tv_sec=st.st_atime},{.tv_sec=st.st_mtime+2}};
    CHECK(utimensat(AT_FDCWD,source_a,times,0)==0);expect_no_cache();
    free(bytes);puts("wallpaper storage: malformed cache/CRC/length/layout/source/generation and removed/changed source rejected");
}
static void test_snapshot_semantics(void) {
    initial_a();struct stat before;CHECK(stat(source_a,&before)==0);
    FILE* file=fopen(source_a,"r+b");CHECK(file && fputc('Z',file)!=EOF && fclose(file)==0);
#ifdef __APPLE__
    struct timespec times[2]={before.st_atimespec,before.st_mtimespec};
#else
    struct timespec times[2]={before.st_atim,before.st_mtim};
#endif
    CHECK(utimensat(AT_FDCWD,source_a,times,0)==0);
    /* Same size/mtime is deliberately only snapshot metadata, not content proof. */
    expect_pixels(small_a,8,4);create_source(source_a,64);
    puts("wallpaper storage: matching metadata retains selection-time snapshot, not current-source identity proof");
}
static void test_sources_and_legacy(void) {
    initial_a();const char* legacy=FOLIO_LEGACY_WALLPAPER_ROOT "/a.png";
    create_source(legacy,64);
    CHECK(folio_storage_wallpaper_save_prepared(legacy,small_b,8,4,error,sizeof(error)));
    expect_path(legacy);expect_pixels(small_b,8,4);
    CHECK(unlink(legacy)==0);expect_no_cache();
    char path[256];CHECK(!folio_storage_wallpaper_load(path,sizeof(path)));
    CHECK(!folio_storage_wallpaper_load_legacy(path,sizeof(path))); /* Never use picture/a.png by basename. */
    create_source(legacy,64);
    CHECK(folio_storage_wallpaper_save(source_a,error,sizeof(error)));expect_no_cache();
    CHECK(folio_storage_wallpaper_load_legacy(path,sizeof(path)) && !strcmp(path,source_a));
    remove_slots();wallpaper_v1_t old={.magic=WALLPAPER_V1_MAGIC,.generation=7};strcpy(old.name,"a.png");
    old.checksum=wallpaper_checksum(&old,offsetof(wallpaper_v1_t,checksum));
    FILE* f=fopen(FOLIO_SD_ROOT "/.folio/wallpaper.a","wb");CHECK(f);
    CHECK(fwrite(&old,1,sizeof(old),f)==sizeof(old) && fclose(f)==0);
    expect_path(legacy);expect_no_cache();CHECK(folio_storage_wallpaper_load_legacy(path,sizeof(path)));
    CHECK(folio_storage_wallpaper_save_prepared(source_a,small_a,8,4,error,sizeof(error)));
    expect_path(source_a);expect_pixels(small_a,8,4);
    /* A migrated identity has no cache alias, even with the same name/gen. */
    wallpaper_record_t record={0};strcpy(record.name,"a.png");record.source=WALLPAPER_MIGRATING;
    FILE* distinct=fopen(legacy,"r+b");CHECK(distinct && fputc('L',distinct)!=EOF && fclose(distinct)==0);
    CHECK(wallpaper_fingerprint(legacy,&record));CHECK(wallpaper_record_write(&record,error,sizeof(error)));
    expect_no_cache();CHECK(folio_storage_wallpaper_load_legacy(path,sizeof(path)));
    create_source(legacy,2u*1024u*1024u+1);read_bytes=0;
    CHECK(!folio_storage_wallpaper_load_legacy(path,sizeof(path)) && read_bytes<2048);
    CHECK(wallpaper_fingerprint(legacy,&record));CHECK(wallpaper_record_write(&record,error,sizeof(error)));
    read_bytes=0;CHECK(!folio_storage_wallpaper_load_legacy(path,sizeof(path)) && read_bytes<2048);
    expect_path(legacy);create_source(legacy,64);
    puts("wallpaper storage: explicit source identity, V1/V2 compatibility and bounded migrating legacy fallback");
}
static void test_faults(void) {
    unsigned totals[IO_KINDS],cancel_total;
    initial_a();CHECK(folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    memcpy(totals,io_calls,sizeof(totals));cancel_total=wallpaper_cancel_calls;
    unsigned cases=0;
    for(unsigned kind=IO_READ;kind<IO_KINDS;++kind)for(unsigned point=1;point<=totals[kind];++point) {
        initial_a();io_kind=kind;io_at=point;
        bool okay=folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error));
        unsigned committed=commit_writes;io_kind=io_at=0;
        if(!okay) {
            if(!committed) {expect_path(source_a);expect_pixels(small_a,8,4);}
            else CHECK(strstr(error,"不确定") && !strstr(error,"原选择保留"));
        }
        /* Whatever is selected after uncertain commit must have a complete matching cache. */
        wallpaper_record_t record;active_record(&record);
        if(!strcmp(record.name,"a.png"))expect_pixels(small_a,8,4);else expect_pixels(small_b,8,4);
        CHECK(!live_count);++cases;
    }
    for(unsigned point=1;point<=cancel_total;++point) {
        initial_a();wallpaper_cancel_at=point;
        bool okay=folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error));
        wallpaper_cancel_at=0;
        if(!okay){CHECK(!commit_writes);expect_path(source_a);expect_pixels(small_a,8,4);}
        ++cases;
    }
    initial_a();CHECK(folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    faults_reset();expect_pixels(small_b,8,4);cancel_total=wallpaper_cancel_calls;
    for(unsigned point=1;point<=cancel_total;++point) {
        faults_reset();wallpaper_cancel_at=point;expect_no_cache();wallpaper_cancel_at=0;expect_pixels(small_b,8,4);++cases;
    }
    printf("wallpaper storage: %u IO/cancel points preserve active snapshot or explicitly report uncertain commit\n",cases);
}
static void test_crashes(void) {
    unsigned totals[IO_KINDS],cancel_total,cases=0;
    initial_a();CHECK(folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    memcpy(totals,io_calls,sizeof(totals));cancel_total=wallpaper_cancel_calls;
    for(unsigned kind=0;kind<IO_KINDS;++kind)for(unsigned point=1;point<=(kind?totals[kind]:cancel_total);++point) {
        initial_a();pid_t child=fork();CHECK(child>=0);
        if(child==0) {
            if(kind){crash_kind=kind;crash_at=point;}else wallpaper_crash_at=point;
            (void)folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error));_exit(0);
        }
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status));
        CHECK(WEXITSTATUS(status)==75 || WEXITSTATUS(status)==76);
        wallpaper_record_t record;active_record(&record);
        if(!strcmp(record.name,"a.png"))expect_pixels(small_a,8,4);else expect_pixels(small_b,8,4);
        /* The read is in another process from the interrupted writer; source bytes are untouched. */
        FILE* a=fopen(source_a,"rb"),*b=fopen(source_b,"rb");CHECK(a && b);
        for(unsigned i=0;i<64;++i)CHECK(fgetc(a)==0 && fgetc(b)==0);
        CHECK(fclose(a)==0 && fclose(b)==0);++cases;
    }
    printf("wallpaper storage: %u process-exit boundaries recover a complete old/new matching snapshot\n",cases);
    initial_a();CHECK(folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    unsigned slot=active_record(NULL);char path[256];snprintf(path,sizeof(path),FOLIO_SD_ROOT "/.folio/wallpaper.%c",'a'+slot);
    FILE* f=fopen(path,"wb");CHECK(f && fputs("torn",f)>=0 && fclose(f)==0);
    expect_path(source_a);expect_pixels(small_a,8,4);
    /* Generation rollover must bind the new slot/cache, not resurrect the old one. */
    wallpaper_record_t record;slot=active_record(&record);record.generation=UINT32_MAX;
    record.checksum=wallpaper_checksum(&record,offsetof(wallpaper_record_t,checksum));
    CHECK(wallpaper_record_commit(&record,slot,error,sizeof(error)));
    CHECK(folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    active_record(&record);CHECK(record.generation==0);expect_pixels(small_b,8,4);
}
static void test_unsafe(const char* outside) {
    initial_a();char path[256];wallpaper_cache_path(1,path);
    CHECK(symlink(outside,path)==0);
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    expect_path(source_a);expect_pixels(small_a,8,4);CHECK(unlink(path)==0);
    CHECK(link(outside,path)==0);
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    expect_path(source_a);expect_pixels(small_a,8,4);CHECK(unlink(path)==0);
    CHECK(mkdir(path,0700)==0);
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    CHECK(rmdir(path)==0);expect_pixels(small_a,8,4);
    CHECK(symlink(source_b,FOLIO_PICTURE_ROOT "/linked.png")==0);
    CHECK(!folio_storage_wallpaper_save_prepared(FOLIO_PICTURE_ROOT "/linked.png",small_b,8,4,error,sizeof(error)));
    CHECK(unlink(FOLIO_PICTURE_ROOT "/linked.png")==0);
    /* Metadata read errors must not misidentify and overwrite the active slot. */
    initial_a();io_kind=IO_READ;io_at=1;
    CHECK(!folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)) && !commit_writes);
    io_kind=io_at=0;expect_path(source_a);expect_pixels(small_a,8,4);
    CHECK(folio_storage_wallpaper_save_prepared(source_b,small_b,8,4,error,sizeof(error)));
    faults_reset();io_kind=IO_READ;io_at=1;expect_no_cache();
    faults_reset();io_kind=IO_READ;io_at=1;char selected[256];
    CHECK(!folio_storage_wallpaper_load_legacy(selected,sizeof(selected)) && !selected[0]);
    faults_reset();expect_path(source_b);expect_pixels(small_b,8,4);
    puts("wallpaper storage: source/cache links, hardlinks, special targets and unreadable selection fail closed");
}
static void test_quota(void) {
    initial_a();pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        signal(SIGXFSZ,SIG_IGN);struct rlimit limit={512,512};CHECK(setrlimit(RLIMIT_FSIZE,&limit)==0);
        uint8_t data[4096];memset(data,0xa5,sizeof(data));
        CHECK(!folio_storage_wallpaper_save_prepared(source_b,data,128,64,error,sizeof(error)));
        CHECK(!commit_writes);expect_path(source_a);expect_pixels(small_a,8,4);_exit(0);
    }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    expect_path(source_a);expect_pixels(small_a,8,4);
    puts("wallpaper storage: real filesystem quota failure leaves active snapshot intact");
}
int main(int argc,char** argv) {
    CHECK(argc==2 && strstr(FOLIO_SD_ROOT,"folio-wallpaper-storage-") && strcmp(FOLIO_SD_ROOT,"/sdcard"));
    memset(small_a,0x12,sizeof(small_a));memset(small_b,0x89,sizeof(small_b));
    CHECK(mkdir(FOLIO_SD_ROOT,0700)==0 && mkdir(FOLIO_PICTURE_ROOT,0700)==0 &&
        mkdir(FOLIO_LEGACY_WALLPAPER_ROOT,0700)==0 && mkdir(FOLIO_SD_ROOT "/.folio",0700)==0);
    create_source(source_a,64);create_source(source_b,64);
    test_bounds();test_full_cache();test_corruption();test_snapshot_semantics();test_sources_and_legacy();test_faults();test_crashes();test_unsafe(argv[1]);test_quota();
    CHECK(!live_bytes && !live_count && !live_files);
    printf("wallpaper storage: %u checks passed; no real SD, image decoding, USB or network\n",checks);return 0;
}
