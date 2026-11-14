#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum { DBG_STATUS, DBG_SCREEN, DBG_TAP, DBG_KEY, DBG_CANCEL } folio_debug_kind_t;
typedef struct { unsigned id; folio_debug_kind_t kind; int x,y; } folio_debug_command_t;
static inline bool folio_debug_number(const char** cursor,unsigned limit,unsigned* value) {
    const char* p=*cursor;while(*p==' ' || *p=='\t')++p;
    if(*p<'0' || *p>'9')return false;
    unsigned n=0;
    while(*p>='0' && *p<='9'){
        unsigned d=(unsigned)(*p++-'0');if(n>limit/10 || (n==limit/10 && d>limit%10))return false;n=n*10+d;
    }
    if(*p && *p!=' ' && *p!='\t')return false;
    *cursor=p;*value=n;return true;
}
static inline bool folio_debug_parse(const char* line,folio_debug_command_t* out) {
    if(strncmp(line,"@FOLIO ",7))return false;
    const char* p=line+7;unsigned id,x=0,y=0;
    if(!folio_debug_number(&p,1000000000u,&id))return false;
    while(*p==' ' || *p=='\t')++p;
    char op[16];size_t len=0;
    while(*p && *p!=' ' && *p!='\t'){if(len+1>=sizeof(op))return false;op[len++]=*p++;}op[len]=0;
    folio_debug_kind_t kind;
    if(!strcmp(op,"TAP")){kind=DBG_TAP;if(!folio_debug_number(&p,683,&x) || !folio_debug_number(&p,1215,&y))return false;}
    else if(!strcmp(op,"KEY")){kind=DBG_KEY;if(!folio_debug_number(&p,2,&x))return false;}
    else if(!strcmp(op,"STATUS"))kind=DBG_STATUS;
    else if(!strcmp(op,"SCREEN"))kind=DBG_SCREEN;
    else if(!strcmp(op,"CANCEL"))kind=DBG_CANCEL;
    else return false;
    while(*p==' ' || *p=='\t')++p;
    if(*p)return false;
    *out=(folio_debug_command_t){id,kind,(int)x,(int)y};return true;
}
static inline uint32_t folio_debug_hash(const uint8_t* bytes,size_t size) {
    uint32_t hash=2166136261u;
    for(size_t i=0;i<size;++i)hash=(hash^bytes[i])*16777619u;
    return hash;
}
/* Pairs of run length and value, independently decodable across chunks. */
static inline size_t folio_debug_rle(const uint8_t* bytes,size_t size,size_t* cursor,uint8_t* out,size_t cap) {
    size_t n=0;
    while(*cursor<size && n+2<=cap){
        uint8_t value=bytes[*cursor];size_t run=1;
        while(run<255 && *cursor+run<size && bytes[*cursor+run]==value)++run;
        out[n++]=(uint8_t)run;out[n++]=value;*cursor+=run;
    }
    return n;
}
