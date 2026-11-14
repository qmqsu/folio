#define _POSIX_C_SOURCE 200809L
#include "folio_weread_time.h"
#include "folio_storage.h"
#include "../book/folio_weread_paths.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { RECORD_BYTES = 128 };
static uint32_t crc32(const void* data,size_t size) {
    uint32_t crc=UINT32_MAX;const unsigned char* p=data;
    while(size--){crc^=*p++;for(unsigned i=0;i<8;++i)crc=(crc>>1)^((0-(crc&1u))&0xedb88320u);}
    return ~crc;
}
static void put32(unsigned char* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t get32(const unsigned char* p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)p[i]<<(8*i);return v;}
static void put64(unsigned char* p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
static uint64_t get64(const unsigned char* p){return get32(p)|((uint64_t)get32(p+4)<<32);}
static uint64_t add(uint64_t a,uint64_t b){return UINT64_MAX-a<b?UINT64_MAX:a+b;}
static bool has_owner(const uint8_t owner[32]) {
    unsigned any=0;for(unsigned i=0;i<32;++i)any|=owner[i];return any!=0;
}
static bool name(const char* path,char suffix,char* out,size_t cap) {
    if(!path)return false;
    size_t n=strnlen(path,FOLIO_STORAGE_PATH_CAP);if(n<5 || n==FOLIO_STORAGE_PATH_CAP || strcmp(path+n-4,".wrb"))return false;
    char canonical[FOLIO_STORAGE_PATH_CAP],error[96];
    if(!folio_storage_normalize(path,canonical,sizeof(canonical),error,sizeof(error)) || strcmp(path,canonical))return false;
    char aux[16];snprintf(aux,sizeof(aux),"time.%c",suffix);
    if(folio_wr_aux(path,aux,out,cap)){
        if(!folio_storage_validate_parent(out,NULL,0))return false;
        char old[288];snprintf(old,sizeof(old),"%s.wrt.%c",path,suffix);
        if(access(out,F_OK)!=0&&errno==ENOENT&&access(old,F_OK)==0){
            if(!folio_storage_validate_parent(old,NULL,0)||rename(old,out))return false;
        }
        return true;
    }
    int k=snprintf(out,cap,"%s.wrt.%c",path,suffix);
    return k>0 && (size_t)k<cap && folio_storage_validate_parent(out,error,sizeof(error));
}
static bool decode(const unsigned char* data,const char* path,folio_weread_time_t* r) {
    if(memcmp(data,"FOLIORT1",8) || get32(data+8)!=1 ||
       get32(data+108)!=crc32(path,strlen(path)) || get32(data+124)!=crc32(data,124) ||
       get32(data+20)>100 || get32(data+64)>300 || get32(data+68)>1 || get32(data+120)>1)return false;
    memset(r,0,sizeof(*r));r->generation=get32(data+12);r->position=get32(data+16);
    r->percent=get32(data+20);r->book_revision=get64(data+24);
    r->pending=get64(data+32);r->confirmed=get64(data+40);r->uncertain=get64(data+48);
    r->inflight=get32(data+64);r->enabled=get32(data+68)!=0;memcpy(r->owner,data+72,32);
    // These bytes were reserved and zero-filled in v1; old records stay valid.
    r->cloud_seconds=get32(data+112);r->cloud_updated_at=get32(data+116);r->cloud_valid=get32(data+120)!=0;
    return r->generation!=0;
}
static bool read_slot(const char* path,char slot,folio_weread_time_t* out,bool* exists) {
    char target[288];if(!name(path,slot,target,sizeof(target))){*exists=true;return false;}
    FILE* file=fopen(target,"rb");if(!file){*exists=errno!=ENOENT;return false;}
    *exists=true;unsigned char bytes[RECORD_BYTES];size_t n=fread(bytes,1,sizeof(bytes),file);
    bool okay=n==sizeof(bytes) && fgetc(file)==EOF && !ferror(file);if(fclose(file))okay=false;
    return okay && decode(bytes,path,out);
}
bool folio_weread_time_load(const char* path,folio_weread_time_t* r) {
    char canonical[256];if(folio_wr_identity(path,canonical,sizeof(canonical)))path=canonical;
    if(!r)return false;
    memset(r,0,sizeof(*r));folio_weread_time_t a,b;bool ea=false,eb=false;
    bool va=read_slot(path,'a',&a,&ea),vb=read_slot(path,'b',&b,&eb);
    // Unlike a reading bookmark, falling back across a corrupt acknowledgement
    // could resurrect already submitted seconds. Fail closed instead.
    if((ea && !va) || (eb && !vb))return false;
    if(!va && !vb)return !ea && !eb;
    *r=va && (!vb || a.generation>=b.generation)?a:b;
    // A crash after the persisted intent may have occurred after server accept.
    // Quarantine this amount, including a lost local acknowledgement.
    if(r->inflight){r->uncertain=add(r->uncertain,r->inflight);r->inflight=0;}
    return true;
}
bool folio_weread_time_save(const char* path,folio_weread_time_t* r) {
    char canonical[256];if(folio_wr_identity(path,canonical,sizeof(canonical)))path=canonical;
    if(!r || r->generation==UINT32_MAX || r->percent>100 || r->inflight>300)return false;
    // Never overwrite a corrupt acknowledgement or roll back a newer writer.
    // A caller cannot turn a failed load into a fresh ledger by ignoring it.
    folio_weread_time_t stored;
    if(!folio_weread_time_load(path,&stored) || stored.generation!=r->generation ||
       (has_owner(stored.owner) && memcmp(stored.owner,r->owner,32)) ||
       (!has_owner(r->owner) && (r->pending || r->inflight || r->cloud_valid)))return false;
    unsigned char bytes[RECORD_BYTES]={0};uint32_t next=r->generation+1;
    memcpy(bytes,"FOLIORT1",8);put32(bytes+8,1);put32(bytes+12,next);
    put32(bytes+16,r->position);put32(bytes+20,r->percent);put64(bytes+24,r->book_revision);
    put64(bytes+32,r->pending);put64(bytes+40,r->confirmed);put64(bytes+48,r->uncertain);
    put32(bytes+64,r->inflight);put32(bytes+68,r->enabled?1:0);memcpy(bytes+72,r->owner,32);
    put32(bytes+112,r->cloud_seconds);put32(bytes+116,r->cloud_updated_at);put32(bytes+120,r->cloud_valid?1:0);
    if(!path)return false;
    put32(bytes+108,crc32(path,strlen(path)));put32(bytes+124,crc32(bytes,124));
    char target[288],temporary[288];
    if(!name(path,(next&1)?'a':'b',target,sizeof(target)) || !name(path,'t',temporary,sizeof(temporary)))return false;
    // Remove an abandoned staging name, then create our own inode exclusively.
    // Never truncate an existing regular file: on a host it could be a hardlink
    // to an unrelated file, even though FAT itself has no hardlinks.
    if(unlink(temporary)!=0 && errno!=ENOENT)return false;
    int descriptor=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);if(descriptor<0)return false;
    FILE* file=fdopen(descriptor,"wb");
    if(!file){close(descriptor);unlink(temporary);return false;}
    bool okay=fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes) && fflush(file)==0 && fsync(fileno(file))==0;
    if(fclose(file))okay=false;
    // FatFS rename cannot replace an existing destination. Only remove the
    // older slot after staging is durable; the latest generation stays intact
    // throughout. If publication fails, its inflight intent still quarantines
    // uncertain submissions on reload instead of resurrecting pending time.
    if(okay)okay=unlink(target)==0 || errno==ENOENT;
    if(okay)okay=rename(temporary,target)==0;
    if(!okay){(void)unlink(temporary);return false;}
    r->generation=next;return true;
}
bool folio_weread_time_bind(folio_weread_time_t* r,const uint8_t owner[32]) {
    if(!r || !owner)return false;
    unsigned existing=0,incoming=0;for(unsigned i=0;i<32;++i){existing|=r->owner[i];incoming|=owner[i];}
    if(!incoming)return false;
    if(existing)return memcmp(r->owner,owner,32)==0;
    memcpy(r->owner,owner,32);return true;
}
void folio_weread_time_add(folio_weread_time_t* r,uint32_t seconds) {
    if(r && r->enabled)r->pending=add(r->pending,seconds);
}
bool folio_weread_time_begin(const char* path,folio_weread_time_t* r) {
    if(!r || !r->enabled || !r->pending || r->inflight || !has_owner(r->owner))return false;
    folio_weread_time_t next=*r;next.inflight=(uint32_t)(next.pending>300?300:next.pending);next.pending-=next.inflight;
    if(!folio_weread_time_save(path,&next))return false;
    *r=next;return true;
}
bool folio_weread_time_finish(const char* path,folio_weread_time_t* r,folio_wr_time_result_t result) {
    if(!r || !r->inflight)return false;
    folio_weread_time_t next=*r;
    if(result==FOLIO_WR_TIME_ACCEPTED)next.confirmed=add(next.confirmed,next.inflight);
    else if(result==FOLIO_WR_TIME_REJECTED)next.pending=add(next.pending,next.inflight);
    else next.uncertain=add(next.uncertain,next.inflight);
    next.inflight=0;
    if(!folio_weread_time_save(path,&next)) {
        // Keep the persisted intent in memory too. No further submit is allowed
        // until reload quarantines it; a failed acknowledgement is never retried.
        return false;
    }
    *r=next;return true;
}
