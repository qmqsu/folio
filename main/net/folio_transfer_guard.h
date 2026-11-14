#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// These pure helpers are shared by the native HTTP server and host tests.
static inline bool folio_transfer_authority(const char* host,char output[48]) {
    size_t n=host?strnlen(host,48):0;
    if(!n || n>=48)return false;
    memcpy(output,host,n+1);
    char* port=strchr(output,':');
    if(port) { if(strcmp(port,":80"))return false; *port=0; }
    return output[0]!=0;
}
static inline bool folio_transfer_host_guard(const char* host,const char* origin,
                                            const char* ap,const char* sta) {
    char request[48], source[48];
    if(!ap || !sta || !folio_transfer_authority(host,request))return false;
    if(strcmp(request,ap) && (!sta[0] || strcmp(request,sta)))return false;
    if(origin && origin[0]) {
        if(strncmp(origin,"http://",7) || !folio_transfer_authority(origin+7,source) || strcmp(source,request))return false;
    }
    return true;
}
static inline bool folio_transfer_token_guard(const char* received,const char* expected) {
    if(!received || !expected || strnlen(received,33)!=32 || strnlen(expected,33)!=32)return false;
    unsigned difference=0;
    for(unsigned i=0;i<32;++i)difference|=(unsigned char)received[i]^(unsigned char)expected[i];
    return difference==0;
}
static inline int folio_transfer_hex(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static inline bool folio_transfer_decode(const char* input,char* output,size_t cap,bool form) {
    size_t used=0;
    if(!input || !output || !cap)return false;
    while(*input) {
        unsigned char c=(unsigned char)*input++;
        if(c=='%') {
            int a=folio_transfer_hex(input[0]),b=input[0]?folio_transfer_hex(input[1]):-1;
            if(a<0||b<0)return false;
            c=(unsigned char)(a*16+b);input+=2;
        } else if(c=='+' && form)c=' ';
        if(!c || used+1>=cap)return false;
        output[used++]=(char)c;
    }
    output[used]=0;return true;
}
