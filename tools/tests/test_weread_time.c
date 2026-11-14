#define _POSIX_C_SOURCE 200809L
#include "folio_weread_time.h"
#include "folio_storage.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

// Match FatFS: unlike POSIX rename, an existing destination is not replaced.
static bool fail_publish;
int folio_test_time_rename(const char* from,const char* to) {
    if(fail_publish){fail_publish=false;errno=EIO;return -1;}
    struct stat st;
    if(!lstat(to,&st)){errno=EEXIST;return -1;}
    if(errno!=ENOENT)return -1;
    return rename(from,to);
}

static const uint8_t owner_a[32]={1,3,5,7},owner_b[32]={2,4,6,8};
static void write_text(const char* path,const char* text) {
    FILE* f=fopen(path,"wb");assert(f);assert(fwrite(text,1,strlen(text),f)==strlen(text));assert(fclose(f)==0);
}
static void book_path(char out[256],const char* name) {
    int n=snprintf(out,256,FOLIO_SD_ROOT "/books/%s.wrb",name);assert(n>0&&n<256);
    write_text(out,"test book pointer");
}
static void sidecar(char out[288],const char* path,char slot) {
    int n=snprintf(out,288,"%s.wrt.%c",path,slot);assert(n>0&&n<288);
}
static folio_weread_time_t fresh(const char* path) {
    folio_weread_time_t r;assert(folio_weread_time_load(path,&r));
    assert(!r.generation&&!r.pending&&!r.inflight&&!r.enabled);
    assert(folio_weread_time_bind(&r,owner_a));r.enabled=true;return r;
}
static void test_owner_position(void) {
    char path[256];book_path(path,"owner");folio_weread_time_t r=fresh(path),loaded;
    assert(folio_weread_time_bind(&r,owner_a));assert(!folio_weread_time_bind(&r,owner_b));
    const uint8_t zero[32]={0};assert(!folio_weread_time_bind(&r,zero));assert(!folio_weread_time_bind(NULL,owner_a));
    r.position=UINT32_C(0xfedcba98);r.percent=87;r.book_revision=UINT64_C(0x123456789abcdef0);
    folio_weread_time_add(&r,75);assert(folio_weread_time_save(path,&r));
    assert(folio_weread_time_load(path,&loaded));
    assert(loaded.position==r.position && loaded.percent==87 && loaded.book_revision==r.book_revision && loaded.pending==75);
    assert(!memcmp(loaded.owner,owner_a,32));
    memcpy(loaded.owner,owner_b,32);assert(!folio_weread_time_save(path,&loaded));
    assert(folio_weread_time_load(path,&loaded));folio_weread_time_t stale=loaded;
    loaded.position=123;loaded.book_revision=456;assert(folio_weread_time_save(path,&loaded));
    assert(!folio_weread_time_save(path,&stale));assert(folio_weread_time_load(path,&r));
    assert(r.position==123 && r.book_revision==456 && r.pending==75);
}
static void test_results(void) {
    char path[256];book_path(path,"responses");folio_weread_time_t r=fresh(path),loaded;
    folio_weread_time_add(&r,650);assert(folio_weread_time_save(path,&r));
    assert(folio_weread_time_begin(path,&r) && r.inflight==300 && r.pending==350);
    assert(!folio_weread_time_begin(path,&r));
    assert(folio_weread_time_finish(path,&r,FOLIO_WR_TIME_ACCEPTED));
    assert(r.confirmed==300 && r.pending==350 && !r.inflight);
    assert(folio_weread_time_begin(path,&r) && r.pending==50);
    assert(folio_weread_time_finish(path,&r,FOLIO_WR_TIME_REJECTED));
    assert(r.pending==350 && r.confirmed==300 && !r.uncertain);
    assert(folio_weread_time_begin(path,&r));folio_weread_time_add(&r,25);
    assert(folio_weread_time_finish(path,&r,FOLIO_WR_TIME_UNKNOWN));
    assert(r.pending==75 && r.confirmed==300 && r.uncertain==300 && !r.inflight);
    assert(folio_weread_time_load(path,&loaded));assert(loaded.pending==75&&loaded.uncertain==300&&loaded.confirmed==300);
    assert(folio_weread_time_begin(path,&loaded) && loaded.inflight==75 && !loaded.pending);
    assert(folio_weread_time_finish(path,&loaded,(folio_wr_time_result_t)99) && loaded.uncertain==375);
    assert(!folio_weread_time_finish(path,&loaded,FOLIO_WR_TIME_ACCEPTED));
}
static void test_cloud_snapshot(void) {
    char path[256];book_path(path,"cloud-snapshot");folio_weread_time_t r=fresh(path),loaded;
    folio_weread_time_add(&r,120);assert(folio_weread_time_save(path,&r));
    assert(folio_weread_time_load(path,&loaded)&&!loaded.cloud_valid);
    // A downloaded total stays separate from local deltas, including repeated
    // downloads and smaller cloud values after a server-side correction.
    r.cloud_seconds=7200;r.cloud_updated_at=1704067200;r.cloud_valid=true;
    assert(folio_weread_time_save(path,&r));
    assert(folio_weread_time_load(path,&loaded)&&loaded.cloud_valid&&loaded.cloud_seconds==7200&&loaded.cloud_updated_at==1704067200);
    assert(loaded.pending==120&&!loaded.confirmed&&!loaded.uncertain&&!loaded.inflight);
    assert(folio_weread_time_save(path,&loaded));
    loaded.cloud_seconds=7100;assert(folio_weread_time_save(path,&loaded));
    assert(folio_weread_time_load(path,&r)&&r.cloud_seconds==7100&&r.pending==120);
    assert(folio_weread_time_begin(path,&r));assert(folio_weread_time_finish(path,&r,FOLIO_WR_TIME_ACCEPTED));
    assert(folio_weread_time_load(path,&loaded)&&loaded.confirmed==120&&loaded.cloud_seconds==7100);
    memcpy(loaded.owner,owner_b,32);assert(!folio_weread_time_save(path,&loaded));
    book_path(path,"cloud-no-owner");memset(&r,0,sizeof(r));r.cloud_valid=true;r.cloud_seconds=1;
    assert(!folio_weread_time_save(path,&r));
}
static void test_crash_and_write_failures(void) {
    char path[256],temporary[288];book_path(path,"crash");folio_weread_time_t r=fresh(path),loaded;
    folio_weread_time_add(&r,300);assert(folio_weread_time_save(path,&r));
    // A real failed temp-file open must leave both memory and disk pending.
    sidecar(temporary,path,'t');assert(mkdir(temporary,0700)==0);
    uint32_t generation=r.generation;assert(!folio_weread_time_begin(path,&r));
    assert(r.generation==generation&&r.pending==300&&!r.inflight);assert(rmdir(temporary)==0);
    assert(folio_weread_time_load(path,&loaded)&&loaded.pending==300&&!loaded.uncertain);
    // A real short write/fflush failure, not a mocked return value.
    struct rlimit previous,limited;assert(getrlimit(RLIMIT_FSIZE,&previous)==0);limited=previous;limited.rlim_cur=64;
    void (*handler)(int)=signal(SIGXFSZ,SIG_IGN);assert(handler!=SIG_ERR);assert(setrlimit(RLIMIT_FSIZE,&limited)==0);
    assert(!folio_weread_time_begin(path,&r) && r.pending==300 && !r.inflight && r.generation==generation);
    assert(setrlimit(RLIMIT_FSIZE,&previous)==0);assert(signal(SIGXFSZ,handler)!=SIG_ERR);
    assert(access(temporary,F_OK)!=0);assert(folio_weread_time_load(path,&loaded)&&loaded.pending==300);
    assert(folio_weread_time_begin(path,&r) && r.inflight==300 && !r.pending);
    // A response received after sending cannot be retried if its local save fails.
    assert(mkdir(temporary,0700)==0);assert(!folio_weread_time_finish(path,&r,FOLIO_WR_TIME_ACCEPTED));
    assert(r.inflight==300&&!r.confirmed&&!folio_weread_time_begin(path,&r));assert(rmdir(temporary)==0);
    assert(folio_weread_time_load(path,&loaded));assert(!loaded.pending&&!loaded.inflight&&loaded.uncertain==300);
    assert(!folio_weread_time_begin(path,&loaded));
    // Reopening repeatedly never resurrects or multiplies quarantined seconds.
    assert(folio_weread_time_load(path,&r)&&r.uncertain==300&&!r.pending);
    assert(folio_weread_time_save(path,&loaded));assert(folio_weread_time_load(path,&r)&&r.uncertain==300&&!r.pending);

    // Failure after removing the old slot must retain the newest intent.
    book_path(path,"publish-failure");r=fresh(path);folio_weread_time_add(&r,60);
    assert(folio_weread_time_save(path,&r));assert(folio_weread_time_begin(path,&r));
    fail_publish=true;assert(!folio_weread_time_finish(path,&r,FOLIO_WR_TIME_ACCEPTED));
    assert(folio_weread_time_load(path,&loaded)&&loaded.uncertain==60&&!loaded.pending);
    assert(!folio_weread_time_begin(path,&loaded));
    assert(folio_weread_time_save(path,&loaded));
}
static void corrupt_byte(const char* path) {
    FILE* f=fopen(path,"r+b");assert(f);assert(fseek(f,40,SEEK_SET)==0);int v=fgetc(f);assert(v!=EOF);
    assert(fseek(f,40,SEEK_SET)==0&&fputc(v^0x80,f)!=EOF&&fclose(f)==0);
}
static void test_corrupt_slots(void) {
    for(unsigned variant=0;variant<4;++variant) {
        char path[256],slot[288],name[32];snprintf(name,sizeof(name),"corrupt-%u",variant);book_path(path,name);
        folio_weread_time_t r=fresh(path),loaded;folio_weread_time_add(&r,60);assert(folio_weread_time_save(path,&r));
        assert(folio_weread_time_begin(path,&r));assert(folio_weread_time_finish(path,&r,FOLIO_WR_TIME_ACCEPTED));
        // A is the new acknowledgement; B is the previous intent. Either bad
        // file must disable the ledger rather than select the remaining slot.
        sidecar(slot,path,variant==1?'b':'a');
        if(variant<2)corrupt_byte(slot);
        else if(variant==2)assert(truncate(slot,20)==0);
        else {FILE* f=fopen(slot,"ab");assert(f&&fputc(0,f)!=EOF&&fclose(f)==0);}
        memset(&loaded,0xaa,sizeof(loaded));assert(!folio_weread_time_load(path,&loaded));
        assert(!loaded.generation&&!loaded.pending&&!loaded.inflight);
        assert(!folio_weread_time_save(path,&r));
        loaded.enabled=true;assert(folio_weread_time_bind(&loaded,owner_a));folio_weread_time_add(&loaded,60);
        assert(!folio_weread_time_begin(path,&loaded));
    }
}
static void test_disabled_saturation(void) {
    char path[256];book_path(path,"saturated");folio_weread_time_t r={0},loaded;
    folio_weread_time_add(&r,20);assert(!r.pending);r.enabled=true;folio_weread_time_add(&r,20);
    assert(!folio_weread_time_begin(path,&r)&&!folio_weread_time_save(path,&r));
    assert(folio_weread_time_bind(&r,owner_a));r.pending=UINT64_MAX-2;folio_weread_time_add(&r,UINT32_MAX);assert(r.pending==UINT64_MAX);
    r.confirmed=UINT64_MAX-2;r.uncertain=UINT64_MAX-2;assert(folio_weread_time_save(path,&r));
    assert(folio_weread_time_begin(path,&r)&&r.inflight==300&&r.pending==UINT64_MAX-300);
    assert(folio_weread_time_finish(path,&r,FOLIO_WR_TIME_ACCEPTED)&&r.confirmed==UINT64_MAX);
    assert(folio_weread_time_begin(path,&r));assert(folio_weread_time_finish(path,&r,FOLIO_WR_TIME_UNKNOWN)&&r.uncertain==UINT64_MAX);
    r.enabled=false;uint64_t before=r.pending;folio_weread_time_add(&r,UINT32_MAX);assert(r.pending==before&&!folio_weread_time_begin(path,&r));
    assert(folio_weread_time_save(path,&r)&&folio_weread_time_load(path,&loaded)&&!loaded.enabled);
    loaded.generation=UINT32_MAX;assert(!folio_weread_time_save(path,&loaded));
    loaded=r;loaded.percent=101;assert(!folio_weread_time_save(path,&loaded));
    loaded=r;loaded.inflight=301;assert(!folio_weread_time_save(path,&loaded));
    assert(!folio_weread_time_load(path,NULL));assert(!folio_weread_time_save(path,NULL));folio_weread_time_add(NULL,20);
}
static void test_paths(const char* outside) {
    char path[256],bad[288],target[288],sentinel[288];book_path(path,"paths");folio_weread_time_t r=fresh(path),loaded;
    snprintf(sentinel,sizeof(sentinel),"%s/sentinel",outside);write_text(sentinel,"PRESERVE");
    const char* fixed[]={NULL,"relative.wrb",FOLIO_SD_ROOT "/../escape.wrb",FOLIO_SD_ROOT "/books/../escape.wrb",FOLIO_SD_ROOT "/books/./paths.wrb",FOLIO_SD_ROOT "/books//paths.wrb",FOLIO_SD_ROOT "/books/evil\\x.wrb",FOLIO_SD_ROOT "/books/no.txt"};
    for(unsigned i=0;i<sizeof(fixed)/sizeof(fixed[0]);++i){assert(!folio_weread_time_save(fixed[i],&r));assert(!folio_weread_time_load(fixed[i],&loaded));}
    snprintf(bad,sizeof(bad),"%s/outside.wrb",outside);assert(!folio_weread_time_save(bad,&r));
    snprintf(bad,sizeof(bad),FOLIO_SD_ROOT "/books/link");assert(symlink(outside,bad)==0);
    snprintf(bad,sizeof(bad),FOLIO_SD_ROOT "/books/link/outside.wrb");assert(!folio_weread_time_save(bad,&r));
    sidecar(target,path,'t');assert(symlink(sentinel,target)==0);assert(!folio_weread_time_save(path,&r));assert(unlink(target)==0);
    sidecar(target,path,'a');assert(symlink(sentinel,target)==0);assert(!folio_weread_time_load(path,&loaded));assert(!folio_weread_time_save(path,&r));assert(unlink(target)==0);
    // Existing staging hardlinks must be unlinked, never truncated in place.
    sidecar(target,path,'t');assert(link(sentinel,target)==0);assert(folio_weread_time_save(path,&r));
    memset(bad,'x',sizeof(bad));bad[sizeof(bad)-1]=0;assert(!folio_weread_time_save(bad,&r));
    char check[9]={0};FILE* f=fopen(sentinel,"rb");assert(f&&fread(check,1,8,f)==8&&!strcmp(check,"PRESERVE")&&fclose(f)==0);
    assert(folio_weread_time_save(path,&r));
    // A copied sidecar cannot become a ledger for a different book path.
    char other[256],source[288];book_path(other,"copy");sidecar(source,path,'a');sidecar(target,other,'a');
    unsigned char bytes[128];f=fopen(source,"rb");assert(f&&fread(bytes,1,sizeof(bytes),f)==sizeof(bytes)&&fclose(f)==0);
    f=fopen(target,"wb");assert(f&&fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes)&&fclose(f)==0);
    assert(!folio_weread_time_load(other,&loaded));
}
int main(int argc,char** argv) {
    assert(argc==2);assert(mkdir(FOLIO_SD_ROOT,0700)==0);assert(mkdir(FOLIO_SD_ROOT "/books",0700)==0);
    assert(folio_storage_lock(100));
    test_owner_position();test_results();test_cloud_snapshot();test_crash_and_write_failures();test_corrupt_slots();test_disabled_saturation();test_paths(argv[1]);
    folio_storage_unlock();puts("WeRead ledger: owners/revisions, acknowledgements, crash quarantine, both corrupt-slot failures, real short writes, saturation and real-storage path guards passed");return 0;
}
