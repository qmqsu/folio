#include "folio_upload.h"
#include "folio_transfer_guard.h"
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char* nonce="00112233445566778899aabbccddeeff";
static bool valid(const char* path,folio_upload_kind_t kind,char* error,size_t cap) {
    (void)kind;FILE* f=fopen(path,"rb");if(!f)return false;
    char bytes[5];bool ok=fread(bytes,1,5,f)==5 && !memcmp(bytes,"hello",5);fclose(f);
    if(!ok && error && cap)snprintf(error,cap,"deliberate invalid fixture");return ok;
}
static void clean(const char* root) {
    const char* dirs[]={"books","fonts","update","picture"};
    for(unsigned i=0;i<4;++i){char path[512];snprintf(path,sizeof(path),"%s/%s",root,dirs[i]);DIR* d=opendir(path);if(!d)continue;struct dirent* e;while((e=readdir(d)))assert(strncmp(e->d_name,".folio-upload-",14));closedir(d);}
}
static void write_file(const char* path,const char* bytes) {FILE* f=fopen(path,"wb");assert(f);assert(fwrite(bytes,1,strlen(bytes),f)==strlen(bytes));assert(fclose(f)==0);}
int main(int argc,char** argv) {
    assert(argc==2);const char* root=argv[1];assert(mkdir(root,0700)==0);
    const char* ap="192.168.4.1";const char* sta="192.168.1.5";
    assert(folio_transfer_host_guard(ap,NULL,ap,sta));
    assert(folio_transfer_host_guard("192.168.4.1:80","http://192.168.4.1",ap,sta));
    assert(folio_transfer_host_guard(ap,"http://192.168.4.1:80",ap,sta));
    assert(folio_transfer_host_guard(sta,"http://192.168.1.5",ap,sta));
    const char* origins[]={"null","https://192.168.4.1","http://evil.test","http://192.168.4.1.evil.test","http://192.168.4.1@evil.test","http://192.168.4.1/","http://192.168.4.1:81","http://192.168.1.5","http://192.168.4.1\r\nX: yes"};
    for(size_t i=0;i<sizeof(origins)/sizeof(origins[0]);++i)assert(!folio_transfer_host_guard(ap,origins[i],ap,sta));
    const char* hosts[]={"evil.test","192.168.4.1.evil.test","192.168.4.1:81","192.168.4.1:80:80","192.168.4.1/","192.168.4.1@evil.test","","[::1]"};
    for(size_t i=0;i<sizeof(hosts)/sizeof(hosts[0]);++i)assert(!folio_transfer_host_guard(hosts[i],NULL,ap,sta));
    assert(!folio_transfer_host_guard(sta,NULL,ap,""));
    assert(folio_transfer_token_guard(nonce,nonce));assert(!folio_transfer_token_guard("",nonce));
    assert(!folio_transfer_token_guard("10112233445566778899aabbccddeeff",nonce));
    assert(!folio_transfer_token_guard("00112233445566778899aabbccddeefe",nonce));
    char decoded[121];assert(folio_transfer_decode("%E4%B9%A6.txt",decoded,sizeof(decoded),false) && !strcmp(decoded,"书.txt"));
    assert(folio_transfer_decode("a+b.txt",decoded,sizeof(decoded),false) && !strcmp(decoded,"a+b.txt"));
    assert(folio_transfer_decode("a+b",decoded,sizeof(decoded),true) && !strcmp(decoded,"a b"));
    const char* encodings[]={"%","%0","%xy","%00","abc%00txt"};
    for(size_t i=0;i<sizeof(encodings)/sizeof(encodings[0]);++i)assert(!folio_transfer_decode(encodings[i],decoded,sizeof(decoded),false));
    assert(!folio_transfer_decode("abcd",decoded,4,false));
    assert(folio_transfer_decode("%2e%2e%2fbook.txt",decoded,sizeof(decoded),false) && !folio_upload_name(decoded,NULL,NULL));
    const char* bad[]={"","../book.txt","/book.txt","sub/book.txt","a\\b.txt",".hidden.txt","x.txt/../y.txt","a:b.txt","a?.txt","a*.txt","a\".txt","a<.txt","a|.txt","x.txt ","x.txt.","CON.txt","CON .txt","aux.epub","com1.ttf","LPT9.jpg","x.html","x.exe","other.bin","FOLIO.BIN","folio.BIN.sha256","\xc0\xaf.txt","\xed\xa0\x80.txt","\xf4\x90\x80\x80.txt","x\n.txt","x\x7f.txt","x\xe2\x82"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!folio_upload_name(bad[i],NULL,NULL));
    const char* good[]={"山间日记.txt","a.EPUB","Latin-font.TTF","b.jpeg","封面.png","folio.bin","folio.bin.sha256"};
    for(size_t i=0;i<sizeof(good)/sizeof(good[0]);++i)assert(folio_upload_name(good[i],NULL,NULL));
    const char* added[]={"常见 文档.PDF","book.MOBI","book.azw3","photo.BMP","photo.GIF","photo.WebP","photo.HEIC","photo.heif"};
    for(size_t i=0;i<sizeof(added)/sizeof(added[0]);++i)assert(folio_upload_name(added[i],NULL,NULL));
    char long_name[201];memset(long_name,'a',196);memcpy(long_name+196,".txt",5);assert(folio_upload_name(long_name,NULL,NULL));
    char too_long[257];memset(too_long,'a',252);memcpy(too_long+252,".txt",5);assert(!folio_upload_name(too_long,NULL,NULL));
    size_t limit;folio_upload_kind_t kind;assert(folio_upload_name("folio.bin",&kind,&limit)&&limit==3u*1024u*1024u && kind==FOLIO_UPLOAD_FIRMWARE);
    assert(!strcmp(folio_upload_directory(kind),"update"));
    assert(folio_upload_in_directory("book.epub","books",NULL));
    assert(folio_upload_in_directory("font.ttf","fonts",NULL));
    assert(folio_upload_name("京华老宋体.ttf",&kind,&limit) && kind==FOLIO_UPLOAD_TTF && limit==32u*1024u*1024u);
    assert(folio_upload_in_directory("folio.bin","update",NULL));
    assert(!folio_upload_in_directory("font.ttf","books",NULL));
    assert(!folio_upload_in_directory("book.epub","update",NULL));
    assert(!folio_upload_in_directory("book.epub","fonts",NULL));
    assert(!folio_upload_in_directory("folio.bin","books",NULL));
    folio_upload_t* u=NULL;char error[128];
    assert(folio_upload_in_directory("图.png","wallpapers",NULL));
    assert(folio_upload_in_directory("图.JPG","wallpapers",NULL));
    assert(!folio_upload_in_directory("folio.bin","wallpapers",NULL));
    assert(!folio_upload_in_directory("book.epub","wallpapers",NULL));
    assert(folio_upload_begin_in_directory(root,"art.png","wallpapers",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);
    assert(folio_upload_write(u,"hello",5)==FOLIO_UPLOAD_OK);
    assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_OK && !u);clean(root);
    char wallpaper[512];snprintf(wallpaper,sizeof(wallpaper),"%s/picture/art.png",root);
    assert(valid(wallpaper,FOLIO_UPLOAD_PNG,error,sizeof(error)));
    assert(folio_upload_begin_in_directory(root,"art.png","wallpapers",5,nonce,valid,&u)==FOLIO_UPLOAD_CONFLICT);
    assert(folio_upload_begin_in_directory(root,"book.txt","wallpapers",5,nonce,valid,&u)==FOLIO_UPLOAD_BAD_NAME);
    assert(folio_upload_begin_in_directory(root,"art.png","../wallpapers",5,nonce,valid,&u)==FOLIO_UPLOAD_BAD_NAME);
    assert(folio_upload_begin_in_directory(root,"art.png","wallpapers",32u*1024u*1024u+1,nonce,valid,&u)==FOLIO_UPLOAD_TOO_LARGE);
    assert(folio_upload_begin(root,"large.png",2u*1024u*1024u+1,nonce,valid,&u)==FOLIO_UPLOAD_OK);folio_upload_abort(&u);clean(root);
    assert(folio_upload_begin(root,"too-big.pdf",32u*1024u*1024u+1,nonce,valid,&u)==FOLIO_UPLOAD_TOO_LARGE);
    assert(folio_upload_begin(root,"folio.bin",3u*1024u*1024u+1,nonce,valid,&u)==FOLIO_UPLOAD_TOO_LARGE);
    assert(folio_upload_begin(root,"ok.txt",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);
    assert(folio_upload_write(u,"he",2)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,"llo",3)==FOLIO_UPLOAD_OK);
    assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_OK && !u);clean(root);
    assert(folio_upload_begin(root,"OK.TXT",5,nonce,valid,&u)==FOLIO_UPLOAD_CONFLICT && !u);
    char path[512];snprintf(path,sizeof(path),"%s/books/ok.txt",root);assert(valid(path,kind,error,sizeof(error)));
    char directory[512];snprintf(directory,sizeof(directory),"%s/books",root);
    snprintf(path,sizeof(path),"%s/books/.folio-upload-%s.part",root,nonce);write_file(path,"interrupted");
    assert(folio_upload_clean_staging(directory));assert(access(path,F_OK)!=0);
    snprintf(path,sizeof(path),"%s/books/.keep.part",root);write_file(path,"keep");assert(folio_upload_clean_staging(directory));assert(access(path,F_OK)==0);
    snprintf(path,sizeof(path),"%s/books/.folio-upload-%s.part",root,nonce);assert(symlink("ok.txt",path)==0);assert(!folio_upload_clean_staging(directory));assert(unlink(path)==0);
    assert(folio_upload_begin(root,"empty.txt",0,nonce,valid,&u)==FOLIO_UPLOAD_TOO_LARGE);
    assert(folio_upload_begin(root,"large.ttf",32u*1024u*1024u,nonce,valid,&u)==FOLIO_UPLOAD_OK);
    folio_upload_abort(&u);clean(root);
    assert(folio_upload_begin(root,"huge.ttf",32u*1024u*1024u+1,nonce,valid,&u)==FOLIO_UPLOAD_TOO_LARGE);
    assert(folio_upload_begin(root,"short.txt",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,"he",2)==FOLIO_UPLOAD_OK);
    assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_TRUNCATED && !u);clean(root);
    assert(folio_upload_begin(root,"excess.txt",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,"hello!",6)==FOLIO_UPLOAD_TOO_LARGE);
    assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_TOO_LARGE);clean(root);
    assert(folio_upload_begin(root,"invalid.txt",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,"bad!!",5)==FOLIO_UPLOAD_OK);
    assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_INVALID && strstr(error,"deliberate"));clean(root);
    assert(folio_upload_begin(root,"cancel.txt",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,"he",2)==FOLIO_UPLOAD_OK);folio_upload_abort(&u);assert(!u);clean(root);
    assert(folio_upload_begin(root,"race.txt",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,"hello",5)==FOLIO_UPLOAD_OK);
    snprintf(path,sizeof(path),"%s/books/race.txt",root);write_file(path,"keep!");assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_CONFLICT);FILE* f=fopen(path,"rb");assert(f);assert(fgetc(f)=='k');fclose(f);clean(root);
    assert(folio_upload_begin(root,"font.ttf",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,"hello",5)==FOLIO_UPLOAD_OK);assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_OK);clean(root);
    assert(folio_upload_begin(root,"folio.bin",5,nonce,valid,&u)==FOLIO_UPLOAD_OK);folio_upload_abort(&u);clean(root);
    // A real POSIX write failure with a small file quota exercises partial-write cleanup.
    pid_t child=fork();assert(child>=0);
    if(!child){signal(SIGXFSZ,SIG_IGN);struct rlimit quota={128,128};assert(setrlimit(RLIMIT_FSIZE,&quota)==0);char bytes[1024];memset(bytes,'x',sizeof(bytes));assert(folio_upload_begin(root,"full.txt",sizeof(bytes),nonce,valid,&u)==FOLIO_UPLOAD_OK);assert(folio_upload_write(u,bytes,sizeof(bytes))==FOLIO_UPLOAD_IO);assert(folio_upload_finish(&u,error,sizeof(error))==FOLIO_UPLOAD_IO);clean(root);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);clean(root);
    // No following a host symlink in a writable category directory.
    snprintf(path,sizeof(path),"%s/fonts/font.ttf",root);assert(unlink(path)==0);snprintf(path,sizeof(path),"%s/fonts",root);assert(rmdir(path)==0);assert(symlink("books",path)==0);
    assert(folio_upload_begin(root,"escape.ttf",5,nonce,valid,&u)==FOLIO_UPLOAD_IO && !u);assert(unlink(path)==0);
    printf("upload: Host/Origin/session/decoding guards, names, bounds, atomic commit, truncation, duplicate/race, invalid data, abort, real quota failure and symlink checks passed\n");return 0;
}
