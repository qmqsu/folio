#include "folio_upload.h"
#include "../book/folio_font.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
__attribute__((weak,noinline)) bool folio_io_cancelled(void) { return false; }

struct folio_upload {
    FILE* file;
    char temporary[FOLIO_UPLOAD_PATH_CAP], destination[FOLIO_UPLOAD_PATH_CAP];
    char directory[FOLIO_UPLOAD_PATH_CAP], name[FOLIO_UPLOAD_NAME_CAP];
    size_t total, received;
    folio_upload_kind_t kind;
    folio_upload_validator_t validate;
    folio_upload_result_t failure;
};

static bool valid_utf8(const unsigned char* p) {
    while (*p) {
        uint32_t cp; unsigned count;
        if (*p < 0x80) { if (*p < 32 || *p == 127) return false; ++p; continue; }
        if (*p >= 0xc2 && *p <= 0xdf) { cp = *p & 31; count = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { cp = *p & 15; count = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { cp = *p & 7; count = 3; }
        else return false;
        ++p;
        for (unsigned i = 0; i < count; ++i, ++p) {
            if ((*p & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (*p & 63);
        }
        if ((count == 2 && cp < 0x800) || (count == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f)) return false;
    }
    return true;
}

bool folio_upload_name(const char* name, folio_upload_kind_t* kind, size_t* limit) {
    if (!name) return false;
    size_t n = strnlen(name, FOLIO_UPLOAD_NAME_CAP);
    if (!n || n >= FOLIO_UPLOAD_NAME_CAP || name[0] == '.' || name[0] == ' ' ||
        name[n-1] == '.' || name[n-1] == ' ' || !valid_utf8((const unsigned char*)name)) return false;
    if (strpbrk(name, "/\\:*?\"<>|")) return false;
    const char* dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    char stem[9] = {0}; size_t stem_len = strcspn(name, ".");
    while(stem_len && name[stem_len-1]==' ')--stem_len;
    if (stem_len < sizeof(stem)) {
        memcpy(stem, name, stem_len);
        if (!strcasecmp(stem,"CON") || !strcasecmp(stem,"PRN") || !strcasecmp(stem,"AUX") ||
            !strcasecmp(stem,"NUL") || (strlen(stem)==4 &&
            (!strncasecmp(stem,"COM",3) || !strncasecmp(stem,"LPT",3)) && stem[3]>='1' && stem[3]<='9')) return false;
    }
    folio_upload_kind_t k; size_t cap;
    if (!strcmp(name,"folio.bin")) { k=FOLIO_UPLOAD_FIRMWARE; cap=3u*1024u*1024u; }
    else if (!strcmp(name,"folio.bin.sha256")) { k=FOLIO_UPLOAD_SHA256; cap=160; }
    else if (!strcasecmp(dot,".txt")) { k=FOLIO_UPLOAD_TXT; cap=16u*1024u*1024u; }
    else if (!strcasecmp(dot,".epub")) { k=FOLIO_UPLOAD_EPUB; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".ttf")) { k=FOLIO_UPLOAD_TTF; cap=FOLIO_FONT_MAX_FILE_BYTES; }
    else if (!strcasecmp(dot,".png")) { k=FOLIO_UPLOAD_PNG; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".jpg") || !strcasecmp(dot,".jpeg")) { k=FOLIO_UPLOAD_JPEG; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".pdf")) { k=FOLIO_UPLOAD_PDF; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".mobi")) { k=FOLIO_UPLOAD_MOBI; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".azw3")) { k=FOLIO_UPLOAD_AZW3; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".bmp")) { k=FOLIO_UPLOAD_BMP; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".gif")) { k=FOLIO_UPLOAD_GIF; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".webp")) { k=FOLIO_UPLOAD_WEBP; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".heic")) { k=FOLIO_UPLOAD_HEIC; cap=32u*1024u*1024u; }
    else if (!strcasecmp(dot,".heif")) { k=FOLIO_UPLOAD_HEIF; cap=32u*1024u*1024u; }
    else return false;
    if (kind) *kind=k;
    if (limit) *limit=cap;
    return true;
}
bool folio_upload_is_image(folio_upload_kind_t kind) {
    return kind==FOLIO_UPLOAD_PNG || kind==FOLIO_UPLOAD_JPEG ||
        (kind>=FOLIO_UPLOAD_BMP && kind<=FOLIO_UPLOAD_HEIF);
}
bool folio_upload_saved_only(folio_upload_kind_t kind) {
    return folio_upload_is_image(kind) || kind==FOLIO_UPLOAD_MOBI || kind==FOLIO_UPLOAD_AZW3;
}
const char* folio_upload_canonical_directory(const char* directory) {
    return directory && !strcmp(directory,"wallpapers")?"picture":directory;
}
const char* folio_upload_directory(folio_upload_kind_t kind) {
    if (folio_upload_is_image(kind)) return "picture";
    if (kind==FOLIO_UPLOAD_TTF) return "fonts";
    if (kind==FOLIO_UPLOAD_FIRMWARE || kind==FOLIO_UPLOAD_SHA256) return "update";
    return "books";
}
bool folio_upload_in_directory(const char* name,const char* directory,folio_upload_kind_t* kind) {
    folio_upload_kind_t parsed;
    if(!directory || !folio_upload_name(name,&parsed,NULL))return false;
    directory=folio_upload_canonical_directory(directory);
    if(strcmp(directory,folio_upload_directory(parsed)) &&
       (strcmp(directory,"books") || (parsed!=FOLIO_UPLOAD_PNG && parsed!=FOLIO_UPLOAD_JPEG)))return false;
    if(kind)*kind=parsed;
    return true;
}

static bool directory_ok(const char* path) {
    struct stat st;
#ifdef ESP_PLATFORM
    return stat(path,&st)==0 && S_ISDIR(st.st_mode);
#else
    return lstat(path,&st)==0 && S_ISDIR(st.st_mode); // no host symlink parents
#endif
}
bool folio_upload_clean_staging(const char* directory) {
    if(!directory || !directory_ok(directory))return false;
    DIR* dir=opendir(directory);if(!dir)return false;
    bool ok=true;struct dirent* entry;
    for(;;) {
        errno=0;entry=readdir(dir);
        if(!entry){if(errno)ok=false;break;}
        if(folio_io_cancelled()){ok=false;break;}
        const char* name=entry->d_name;
        if(strlen(name)!=51 || strncmp(name,".folio-upload-",14) || strcmp(name+46,".part") || strspn(name+14,"0123456789abcdef")!=32)continue;
        char path[FOLIO_UPLOAD_PATH_CAP];int n=snprintf(path,sizeof(path),"%s/%.51s",directory,name);
        struct stat st;
        if(n<0 || (size_t)n>=sizeof(path)){ok=false;break;}
#ifdef ESP_PLATFORM
        int found=stat(path,&st);
#else
        int found=lstat(path,&st);
#endif
        if(found!=0 || !S_ISREG(st.st_mode) || unlink(path)!=0){ok=false;break;}
    }
    if(closedir(dir)!=0)ok=false;
    return ok;
}
static int exists(const folio_upload_t* u) {
    struct stat st;
#ifdef ESP_PLATFORM
    int found=stat(u->destination,&st);
#else
    int found=lstat(u->destination,&st);
#endif
    if(found==0)return 1; // also respects the mounted filesystem's case aliases
    if(errno!=ENOENT)return -1;
    DIR* dir=opendir(u->directory);
    if (!dir) return -1;
    int result=0; struct dirent* entry;
    errno=0;
    while ((entry=readdir(dir))) {
        if(folio_io_cancelled()){result=-1;break;}
        if (!strcasecmp(entry->d_name,u->name)) { result=1; break; }
    }
    if (!entry && errno) result=-1;
    if (closedir(dir)!=0) result=-1;
    return result;
}
folio_upload_result_t folio_upload_begin(const char* root, const char* name, size_t total,
    const char* nonce, folio_upload_validator_t validate, folio_upload_t** out) {
    return folio_upload_begin_in_directory(root,name,NULL,total,nonce,validate,out);
}
folio_upload_result_t folio_upload_begin_in_directory(const char* root,const char* name,const char* directory,size_t total,
    const char* nonce,folio_upload_validator_t validate,folio_upload_t** out) {
    if (!out) return FOLIO_UPLOAD_IO;
    *out=NULL;
    folio_upload_kind_t kind; size_t limit;
    if (!root || !validate || !folio_upload_name(name,&kind,&limit)) return FOLIO_UPLOAD_BAD_NAME;
    if(!directory)directory=folio_upload_directory(kind);
    directory=folio_upload_canonical_directory(directory);
    if(!folio_upload_in_directory(name,directory,NULL) ||
       strlen(root)+1+strlen(directory)+1+strlen(name)>=FOLIO_UPLOAD_PATH_CAP)return FOLIO_UPLOAD_BAD_NAME;
    if (!total || total>limit) return FOLIO_UPLOAD_TOO_LARGE;
    if (!nonce || strlen(nonce)!=32 || strspn(nonce,"0123456789abcdef")!=32) return FOLIO_UPLOAD_BAD_NAME;
    folio_upload_t* u=calloc(1,sizeof(*u)); if (!u) return FOLIO_UPLOAD_IO;
    u->kind=kind; u->total=total; u->validate=validate;
    snprintf(u->name,sizeof(u->name),"%s",name);
    int n=snprintf(u->directory,sizeof(u->directory),"%s/%s",root,directory);
    if (n<0 || (size_t)n>=sizeof(u->directory) || !directory_ok(root)) goto io_error;
    if (mkdir(u->directory,0777)!=0 && errno!=EEXIST) goto io_error;
    if (!directory_ok(u->directory)) goto io_error;
    n=snprintf(u->destination,sizeof(u->destination),"%s/%s",u->directory,name);
    if (n<0 || (size_t)n>=sizeof(u->destination)) goto io_error;
    n=snprintf(u->temporary,sizeof(u->temporary),"%s/.folio-upload-%s.part",u->directory,nonce);
    if (n<0 || (size_t)n>=sizeof(u->temporary)) goto io_error;
    int present=exists(u);
    if (present<0) goto io_error;
    if (present) { free(u); return FOLIO_UPLOAD_CONFLICT; }
    int fd=open(u->temporary,O_CREAT|O_EXCL|O_WRONLY,0600);
    if (fd<0) goto io_error;
    u->file=fdopen(fd,"wb");
    if (!u->file) { close(fd); unlink(u->temporary); goto io_error; }
    setvbuf(u->file,NULL,_IONBF,0);
    *out=u; return FOLIO_UPLOAD_OK;
io_error:
    free(u); return FOLIO_UPLOAD_IO;
}
folio_upload_result_t folio_upload_write(folio_upload_t* u, const void* bytes, size_t count) {
    if (!u || !u->file) return FOLIO_UPLOAD_IO;
    if (u->failure) return u->failure;
    if (count>u->total-u->received || (!bytes && count)) return u->failure=FOLIO_UPLOAD_TOO_LARGE;
    if (count && fwrite(bytes,1,count,u->file)!=count) return u->failure=FOLIO_UPLOAD_IO;
    u->received+=count; return FOLIO_UPLOAD_OK;
}
void folio_upload_abort(folio_upload_t** ptr) {
    if (!ptr || !*ptr) return;
    folio_upload_t* u=*ptr; *ptr=NULL;
    if (u->file) fclose(u->file);
    if (u->temporary[0]) unlink(u->temporary);
    free(u);
}
folio_upload_result_t folio_upload_finish(folio_upload_t** ptr, char* error, size_t cap) {
    if (error && cap) error[0]=0;
    if (!ptr || !*ptr) return FOLIO_UPLOAD_IO;
    folio_upload_t* u=*ptr;
    folio_upload_result_t result=u->failure;
    if (!result && u->received!=u->total) result=FOLIO_UPLOAD_TRUNCATED;
    if (fflush(u->file)!=0 || fsync(fileno(u->file))!=0) result=FOLIO_UPLOAD_IO;
    if (fclose(u->file)!=0) result=FOLIO_UPLOAD_IO;
    u->file=NULL;
    if (!result && folio_io_cancelled()) result=FOLIO_UPLOAD_TRUNCATED;
    if (!result && !u->validate(u->temporary,u->kind,error,cap))
        result=folio_io_cancelled()?FOLIO_UPLOAD_TRUNCATED:FOLIO_UPLOAD_INVALID;
    if (!result) {
        int present=exists(u);
        if (present<0) result=FOLIO_UPLOAD_IO;
        else if (present) result=FOLIO_UPLOAD_CONFLICT;
        else if (folio_io_cancelled()) result=FOLIO_UPLOAD_TRUNCATED;
        else if (rename(u->temporary,u->destination)!=0) result=FOLIO_UPLOAD_IO;
        else u->temporary[0]=0;
    }
    folio_upload_abort(ptr);
    if (result && error && cap && !error[0]) snprintf(error,cap,"%s",folio_upload_message(result));
    return result;
}
const char* folio_upload_message(folio_upload_result_t result) {
    switch(result) {
        case FOLIO_UPLOAD_OK:return "上传完成。";
        case FOLIO_UPLOAD_BAD_NAME:return "文件名无效或文件类型暂不支持。";
        case FOLIO_UPLOAD_TOO_LARGE:return "文件为空、超过大小上限或传输长度不符。";
        case FOLIO_UPLOAD_CONFLICT:return "已有同名文件，请重命名后上传。";
        case FOLIO_UPLOAD_INVALID:return "文件损坏或格式暂不支持。";
        case FOLIO_UPLOAD_TRUNCATED:return "上传已中断，未完成文件已清理。";
        default:return "写入失败，请检查 TF 卡和剩余空间。";
    }
}
