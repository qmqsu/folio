#include "folio_upload.h"
#include "folio_cover.h"
#include "folio_update.h"
#include "folio_epub.h"
#include "esp_ota_ops.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned cancel_after,calls;
bool fail_crypto_init,fail_hash_setup,fail_hash_finish;
void* heap_caps_malloc(size_t bytes,unsigned caps) { (void)caps;return malloc(bytes); }
bool folio_io_cancelled(void) { return cancel_after && ++calls>=cancel_after; }
// Flash operations are forbidden during staging validation.
const esp_partition_t* esp_ota_get_running_partition(void){assert(false);return NULL;}
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* p){(void)p;assert(false);return NULL;}
const esp_partition_t* esp_partition_find_first(unsigned t,unsigned s,const char* n){(void)t;(void)s;(void)n;assert(false);return NULL;}
esp_err_t esp_ota_begin(const esp_partition_t* p,size_t n,esp_ota_handle_t* h){(void)p;(void)n;(void)h;assert(false);return ESP_FAIL;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void* p,size_t n){(void)h;(void)p;(void)n;assert(false);return ESP_FAIL;}
esp_err_t esp_ota_end(esp_ota_handle_t h){(void)h;assert(false);return ESP_FAIL;}
esp_err_t esp_ota_abort(esp_ota_handle_t h){(void)h;assert(false);return ESP_FAIL;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p){(void)p;assert(false);return ESP_FAIL;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p,esp_ota_img_states_t* s){(void)p;(void)s;assert(false);return ESP_FAIL;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void){assert(false);return ESP_FAIL;}
void epd_draw_pixel(int x,int y,uint8_t shade,uint8_t* fb) {(void)x;(void)y;(void)shade;(void)fb;}
int epd_rotated_display_width(void){return 684;}
int epd_rotated_display_height(void){return 1216;}
static void no_temporary(const char* root) {
    const char* dirs[]={"books","fonts","update","picture"};
    for(unsigned i=0;i<4;++i){char path[512];snprintf(path,sizeof(path),"%s/%s",root,dirs[i]);DIR* d=opendir(path);if(!d)continue;struct dirent* e;while((e=readdir(d)))assert(strncmp(e->d_name,".folio-upload-",14));closedir(d);}
}
static void upload(const char* root,const char* source,const char* name,bool expected) {
    unsigned target_cancel=cancel_after;cancel_after=0;
    FILE* f=fopen(source,"rb");assert(f);assert(fseek(f,0,SEEK_END)==0);long length=ftell(f);assert(length>0);rewind(f);
    folio_upload_t* u=NULL;assert(folio_upload_begin(root,name,(size_t)length,"1234567890abcdef1234567890abcdef",folio_transfer_validate,&u)==FOLIO_UPLOAD_OK);
    char chunk[353];size_t n;while((n=fread(chunk,1,sizeof(chunk),f)))assert(folio_upload_write(u,chunk,n)==FOLIO_UPLOAD_OK);assert(!ferror(f));fclose(f);
    calls=0;cancel_after=target_cancel;
    char error[128];folio_upload_result_t result=folio_upload_finish(&u,error,sizeof(error));assert(!u);
    if((result==FOLIO_UPLOAD_OK)!=expected){fprintf(stderr,"Unexpected %s: %d %s\n",name,result,error);abort();}
    no_temporary(root);
    if(expected) {
        folio_upload_kind_t kind;assert(folio_upload_name(name,&kind,NULL));
        char target[512];snprintf(target,sizeof(target),"%s/%s/%s",root,folio_upload_directory(kind),name);
        FILE* original=fopen(source,"rb"),*saved=fopen(target,"rb");assert(original && saved);
        int a,b;do{a=fgetc(original);b=fgetc(saved);assert(a==b);}while(a!=EOF);
        assert(!ferror(original) && !ferror(saved));fclose(original);fclose(saved);
        assert(folio_transfer_saved_only(target,kind)==(folio_upload_saved_only(kind) || !strncmp(name,"utf16",5) || !strncmp(name,"utf32",5)));
    }
    char cache[512];snprintf(cache,sizeof(cache),"%s/.folio/cache",root);DIR* dir=opendir(cache);
    if(dir){struct dirent* e;while((e=readdir(dir)))assert(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."));closedir(dir);}
}
int main(int argc,char** argv){
    assert(argc==3);const char* root=argv[1];const char* fixtures=argv[2];assert(mkdir(root,0700)==0);
    struct {const char* fixture;const char* name;bool valid;} cases[]={
        {"replacement.txt","replacement.txt",true},{"good.txt","utf8.txt",true},{"gbk.txt","gbk.txt",true},{"good.epub","book.epub",true},
        {"good.ttf","font.ttf",true},{"good.png","cover.png",true},{"good.jpg","cover.jpg",true},
        {"binary.txt","fake.txt",false},{"broken-utf8.txt","broken.txt",false},{"utf16.txt","utf16.txt",true},
        {"blank.txt","blank.txt",false},{"bad.epub","bad.epub",false},{"broken.epub","truncated.epub",false},
        {"binary.txt","fake.ttf",false},{"broken.ttf","broken.ttf",false},{"good.jpg","fake.png",false},
        {"broken.png","broken.png",false},{"broken.jpg","broken.jpg",false},
        {"good.bin","folio.bin",true},{"good.sha256","folio.bin.sha256",true},{"bad.sha256","bad.txt",true},
        {"utf16be.txt","utf16be.txt",true},{"utf32le.txt","utf32le.txt",true},{"utf32be.txt","utf32be.txt",true},
        {"bad-surrogate.txt","bad-surrogate.txt",false},{"odd-utf16.txt","odd-utf16.txt",false},{"bad-utf32.txt","bad-utf32.txt",false},
        {"large-pixels.png","large-pixels.png",true},{"large-bytes.png","large-bytes.png",true},{"depth16.png","depth16.png",true},{"interlaced.png","interlaced.png",true},
        {"large.jpg","large.jpg",true},{"bad-crc.png","bad-crc.png",false},{"no-eoi.jpg","no-eoi.jpg",false},
        {"good.pdf","Document.PDF",true},{"bad.pdf","bad.pdf",false},{"binary.txt","fake.pdf",false},
        {"good.mobi","Book.MOBI",true},{"good.mobi","Book.AZW3",true},{"bad.mobi","bad.mobi",false},{"binary.txt","fake.azw3",false},
        {"good.gif","Image.GIF",true},{"bad.gif","bad.gif",false},{"good.bmp","Image.BMP",true},
        {"good.webp","Image.WebP",true},{"good.heic","Image.HEIC",true},{"good.heic","Image.HEIF",true},{"bad.heic","bad.heic",false},
        {"binary.txt","fake.gif",false},{"binary.txt","fake.bmp",false},{"binary.txt","fake.webp",false},{"binary.txt","fake.heif",false},
        {"missing-mime.epub","missing-mime.epub",true},{"wrong-mime.epub","wrong-mime.epub",false},
        {"long-chapter.epub","long-chapter.epub",true},{"traversal.epub","traversal.epub",false},{"crc.epub","crc.epub",false}
    };
    char path[512];
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){snprintf(path,sizeof(path),"%s/%s",fixtures,cases[i].fixture);upload(root,path,cases[i].name,cases[i].valid);}
    const char* hashes[]={"bad.sha256","short.sha256","foreign.sha256","nul.sha256"};char error[128];
    for(unsigned i=0;i<4;++i){snprintf(path,sizeof(path),"%s/%s",fixtures,hashes[i]);assert(!folio_transfer_validate(path,FOLIO_UPLOAD_SHA256,error,sizeof(error)));}
    snprintf(path,sizeof(path),"%s/good.txt",fixtures);assert(!folio_transfer_validate(path,FOLIO_UPLOAD_FIRMWARE,error,sizeof(error)));
    // Cancellation after work has begun, not only an early entry guard.
    const char* cancel_files[]={"good.ttf","good.png","good.epub","good.txt"};
    const char* cancel_names[]={"cancel.ttf","cancel.png","cancel.epub","cancel.txt"};
    for(unsigned i=0;i<4;++i){snprintf(path,sizeof(path),"%s/%s",fixtures,cancel_files[i]);calls=0;cancel_after=3;upload(root,path,cancel_names[i],false);assert(calls>=3);cancel_after=0;}
    snprintf(path,sizeof(path),"%s/good.bin",fixtures);calls=0;cancel_after=3;assert(!folio_transfer_validate(path,FOLIO_UPLOAD_FIRMWARE,error,sizeof(error)));assert(calls>=3);cancel_after=0;
    snprintf(path,sizeof(path),"%s/bad.bin",fixtures);assert(!folio_transfer_validate(path,FOLIO_UPLOAD_FIRMWARE,error,sizeof(error)));
    // The JPEG hook interrupts the actual entropy row loop, then normal cleanup
    // leaves the bounded decoder allocator reusable for a successful retry.
    const unsigned jpeg_cancel_points[]={4,12,60};
    const char* jpeg_files[]={"good.jpg","gray.jpg","progressive.jpg"};
    for(unsigned j=0;j<3;++j){snprintf(path,sizeof(path),"%s/%s",fixtures,jpeg_files[j]);assert(folio_cover_validate_file(path));
        for(unsigned i=0;i<3;++i){calls=0;cancel_after=jpeg_cancel_points[i];assert(!folio_cover_validate_file(path));assert(calls==cancel_after);cancel_after=0;assert(folio_cover_validate_file(path));}}
    // Validation preserves a pre-existing reader cache, even when its key is
    // identical to the staging key. Only newly generated validation caches go.
    snprintf(path,sizeof(path),"%s/good.epub",fixtures);folio_epub_t existing={0};
    assert(folio_epub_prepare(path,&existing,error,sizeof(error)));
    assert(folio_transfer_validate(path,FOLIO_UPLOAD_EPUB,error,sizeof(error)));
    struct stat cache_stat;assert(stat(existing.text_path,&cache_stat)==0 && cache_stat.st_size>0);
    assert(remove(existing.text_path)==0);folio_epub_free(&existing);
    // A valid oversized chapter is safely saved but not promised readable.
    snprintf(path,sizeof(path),"%s/books/long-chapter.epub",root);folio_epub_t oversized={0};
    assert(!folio_epub_prepare(path,&oversized,error,sizeof(error)));folio_epub_free(&oversized);
    folio_cover_clear_cache();puts("transfer validation: UTF8/GBK/U+FFFD/UTF16/32, CRC-safe EPUB/large chapter, TTF, large PNG/JPEG, PDF/MOBI/AZW3/BMP/GIF/WebP/HEIF containers, byte preservation, damage/truncation/cancellation and unchanged OTA staging passed");return 0;
}
