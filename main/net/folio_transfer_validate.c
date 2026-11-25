#include "folio_upload.h"
#include "folio_font.h"
#include "folio_text.h"
#include "folio_update.h"
#include "folio_zip.h"
#include "folio_xml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FOLIO_EPUB_HOST_TEST
#include <zlib.h>
#else
#include "miniz.h"
#endif
extern bool folio_io_cancelled(void);
static bool cancelled(void) { return folio_io_cancelled(); }
static uint16_t le16(const unsigned char* p) { return p[0]|((uint16_t)p[1]<<8); }
static uint32_t le32(const unsigned char* p) { return le16(p)|((uint32_t)le16(p+2)<<16); }
static uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static bool read_at(FILE* f,size_t offset,void* bytes,size_t count) {
    return !cancelled() && fseek(f,(long)offset,SEEK_SET)==0 && fread(bytes,1,count,f)==count;
}

// Verify every ZIP member without allocating its expanded size or creating a
// reader cache. Rendering/manifest/spine/chapter limits are not transfer limits.
static bool zip_member(const folio_zip_t* zip,const folio_zip_entry_t* entry) {
    unsigned char* input=folio_epub_alloc(4096),*output=folio_epub_alloc(32768);
    if(!input || !output){free(input);free(output);return false;}
    bool ok=false;uint32_t remaining=entry->compressed,produced_total=0,crc=0;
    size_t pos=0,count=0,ring=0;
    if(fseek(zip->file,(long)entry->data_offset,SEEK_SET)!=0)goto end;
    if(entry->method==0) {
        while(remaining && !cancelled()) {
            size_t n=remaining>4096?4096:remaining;
            if(fread(input,1,n,zip->file)!=n)goto end;
            crc=folio_epub_crc32(crc,input,n);remaining-=(uint32_t)n;folio_epub_yield();
        }
        ok=!remaining && !cancelled() && crc==entry->crc;goto end;
    }
#ifdef FOLIO_EPUB_HOST_TEST
    z_stream state={0};if(inflateInit2(&state,-15)!=Z_OK)goto end;
#else
    tinfl_decompressor* state=folio_epub_alloc(sizeof(*state));if(!state)goto end;
    memset(state,0,sizeof(*state));tinfl_init(state);
#endif
    while(!cancelled()) {
        if(pos==count && remaining) {
            count=remaining>4096?4096:remaining;pos=0;
            if(fread(input,1,count,zip->file)!=count)break;
            remaining-=(uint32_t)count;
        }
        size_t in=count-pos,out=32768-ring,consumed,produced;bool done,failed;
#ifdef FOLIO_EPUB_HOST_TEST
        state.next_in=input+pos;state.avail_in=(uInt)in;
        state.next_out=output+ring;state.avail_out=(uInt)out;
        int result=inflate(&state,Z_NO_FLUSH);
        consumed=in-state.avail_in;produced=out-state.avail_out;
        done=result==Z_STREAM_END;failed=result<0;
#else
        int result=tinfl_decompress(state,input+pos,&in,output,output+ring,&out,
            remaining?TINFL_FLAG_HAS_MORE_INPUT:0);
        consumed=in;produced=out;done=result==TINFL_STATUS_DONE;failed=result<TINFL_STATUS_DONE;
#endif
        if(produced>entry->size-produced_total)break;
        crc=folio_epub_crc32(crc,output+ring,produced);produced_total+=(uint32_t)produced;
        ring=(ring+produced)&32767;pos+=consumed;
        if(done){ok=produced_total==entry->size && pos==count && !remaining && crc==entry->crc;break;}
        if(failed || (!consumed && !produced))break;
        folio_epub_yield();
    }
#ifdef FOLIO_EPUB_HOST_TEST
    inflateEnd(&state);
#else
    free(state);
#endif
end:
    free(input);free(output);return ok;
}
typedef struct { char package[256];bool root,manifest,spine,item; } epub_check_t;
static bool container_token(void* context,const folio_xml_token_t* token) {
    epub_check_t* check=context;
    if(token->kind==FOLIO_XML_START && !token->depth && !folio_xml_name(token->name,"container"))return false;
    if(token->kind==FOLIO_XML_START && folio_xml_name(token->name,"rootfile") && !check->package[0])
        return folio_xml_attr(token,"full-path",check->package,sizeof(check->package));
    return !cancelled();
}
static bool package_token(void* context,const folio_xml_token_t* token) {
    epub_check_t* check=context;
    if(token->kind==FOLIO_XML_START) {
        if(!token->depth){check->root=folio_xml_name(token->name,"package");if(!check->root)return false;}
        if(folio_xml_name(token->name,"manifest"))check->manifest=true;
        if(folio_xml_name(token->name,"spine"))check->spine=true;
        if(folio_xml_name(token->name,"itemref"))check->item=true;
    }
    return !cancelled();
}
static bool validate_epub(const char* path,char* error,size_t cap) {
    folio_zip_t zip={0};if(!folio_zip_open_directory(path,&zip,error,cap))return false;
    bool ok=true;uint64_t expanded=0;
    for(size_t i=0;ok && i<zip.count;++i) {
        expanded+=zip.entries[i].size;
        // Bounded decompression budget, independent of reader text conversion.
        ok=expanded<=128u*1024u*1024u && zip_member(&zip,&zip.entries[i]);
    }
    size_t size=0;unsigned char* data=NULL;epub_check_t check={0};
    if(ok) ok=folio_zip_check_epub_type(&zip,error,cap);
    if(ok) {data=folio_zip_read(&zip,folio_zip_find(&zip,"META-INF/container.xml"),FOLIO_EPUB_XML_LIMIT,&size,error,cap);
        ok=data && folio_xml_parse((char*)data,size,container_token,&check) && check.package[0];free(data);}
    if(ok) {data=folio_zip_read(&zip,folio_zip_find(&zip,check.package),FOLIO_EPUB_XML_LIMIT,&size,error,cap);
        ok=data && folio_xml_parse((char*)data,size,package_token,&check) && check.root && check.manifest && check.spine && check.item;free(data);}
    folio_zip_close(&zip);return ok;
}

// Container integrity checks do not invoke a display decoder, impose pixel
// limits, execute content or promise that the saved image/book can be opened.
static bool png_file(FILE* file,size_t size) {
    unsigned char h[33],buf[4096];
    if(size<57 || !read_at(file,0,h,33) || memcmp(h,"\x89PNG\r\n\x1a\n",8) ||
       be32(h+8)!=13 || memcmp(h+12,"IHDR",4) || !be32(h+16) || !be32(h+20))return false;
    unsigned depth=h[24],color=h[25];
    if(!((color==0 && (depth==1||depth==2||depth==4||depth==8||depth==16)) ||
         (color==3 && (depth==1||depth==2||depth==4||depth==8)) ||
         ((color==2||color==4||color==6) && (depth==8||depth==16))) || h[26] || h[27] || h[28]>1)return false;
    size_t pos=8;bool idat=false,palette=false;
    while(pos<size && !cancelled()) {
        if(size-pos<12 || !read_at(file,pos,h,8))return false;
        size_t length=be32(h);if(length>size-pos-12)return false;
        if(pos!=8 && !memcmp(h+4,"IHDR",4))return false;
        if(!memcmp(h+4,"PLTE",4)){if(!length || length>768 || length%3)return false;palette=true;}
        if(!memcmp(h+4,"IDAT",4))idat=true;
        bool end=!memcmp(h+4,"IEND",4);
        uint32_t crc=folio_epub_crc32(0,h+4,4);size_t remaining=length;
        while(remaining) {
            size_t n=remaining>sizeof(buf)?sizeof(buf):remaining;
            if(cancelled() || fread(buf,1,n,file)!=n)return false;
            crc=folio_epub_crc32(crc,buf,n);remaining-=n;folio_epub_yield();
        }
        if(fread(h,1,4,file)!=4 || be32(h)!=crc)return false;
        pos+=length+12;
        if(end)return !length && idat && (color!=3 || palette) && pos==size;
    }
    return false;
}
static bool jpeg_file(FILE* file,size_t size) {
    unsigned char h[8];if(size<12 || !read_at(file,0,h,2) || h[0]!=255 || h[1]!=216)return false;
    size_t pos=2;bool frame=false,scan=false,entropy=false;
    while(pos<size) {
        if((!entropy || !(pos&4095)) && cancelled())return false;
        int c=fgetc(file);++pos;
        if(c!=255){if(!entropy || c==EOF)return false;if(!(pos&4095))folio_epub_yield();continue;}
        do {c=fgetc(file);++pos;}while(c==255 && pos<size);
        if(c==EOF)return false;
        if(entropy && (c==0 || (c>=208 && c<=215)))continue;
        if(c==217)return frame && scan; // Camera trailers after EOI are allowed.
        if(c==216 || c==0 || c==1 || (c>=208&&c<=215))return false;
        entropy=false;
        if(size-pos<2 || fread(h,1,2,file)!=2)return false;
        size_t length=((size_t)h[0]<<8)|h[1];
        if(length<2 || length>size-pos)return false;
        if(c>=192 && c<=207 && c!=196 && c!=200 && c!=204) {
            if(length<8 || fread(h,1,6,file)!=6 || !(h[1]|h[2]) || !(h[3]|h[4]) || !h[5])return false;
            frame=true;
        }
        if(c==218){if(!frame || length<6)return false;scan=true;entropy=true;}
        pos+=length;
        if(fseek(file,(long)pos,SEEK_SET)!=0)return false;
    }
    return false;
}
static bool gif_blocks(FILE* f,size_t size,size_t* pos) {
    unsigned char n;
    do {if(*pos>=size || !read_at(f,(*pos)++,&n,1) || n>size-*pos)return false;*pos+=n;}while(n);
    return true;
}
static bool gif_file(FILE* f,size_t size,const unsigned char* h) {
    if(size<14 || (memcmp(h,"GIF87a",6)&&memcmp(h,"GIF89a",6)) || !le16(h+6) || !le16(h+8))return false;
    size_t pos=13+((h[10]&128)?3u<<(1+(h[10]&7)):0);bool image=false;unsigned char b[10];
    while(pos<size && !cancelled()) {
        if(!read_at(f,pos++,b,1))return false;
        if(b[0]==0x3b)return image && pos==size;
        if(b[0]==0x21){if(pos>=size)return false;++pos;if(!gif_blocks(f,size,&pos))return false;}
        else if(b[0]==0x2c) {
            if(size-pos<9 || !read_at(f,pos,b,9) || !le16(b+4) || !le16(b+6))return false;
            pos+=9+((b[8]&128)?3u<<(1+(b[8]&7)):0);
            if(pos>=size || !read_at(f,pos++,b,1) || b[0]<2 || b[0]>8 || !gif_blocks(f,size,&pos))return false;
            image=true;
        } else return false;
    }
    return false;
}
static bool webp_file(FILE* f,size_t size,const unsigned char* h) {
    if(size<20 || memcmp(h,"RIFF",4) || memcmp(h+8,"WEBP",4) || (uint64_t)le32(h+4)+8!=size)return false;
    size_t pos=12;bool image=false;unsigned char b[8];
    while(pos<size) {
        if(size-pos<8 || !read_at(f,pos,b,8))return false;
        size_t n=le32(b+4);if(n>size-pos-8 || (n&1)>size-pos-8-n)return false;
        if((!memcmp(b,"VP8 ",4)&&n>=10) || (!memcmp(b,"VP8L",4)&&n>=5) || (!memcmp(b,"ANMF",4)&&n>=16))image=true;
        pos+=8+n+(n&1);
    }
    return image;
}
static bool heif_file(FILE* f,size_t size) {
    size_t pos=0;bool brand=false,meta=false,data=false;unsigned char h[16];
    while(pos<size) {
        if(size-pos<8 || !read_at(f,pos,h,8))return false;
        uint64_t length=be32(h);size_t header=8;
        if(length==1){if(size-pos<16 || !read_at(f,pos+8,h+8,8))return false;length=((uint64_t)be32(h+8)<<32)|be32(h+12);header=16;}
        if(!length)length=size-pos;
        if(length<header || length>size-pos)return false;
        if(!memcmp(h+4,"ftyp",4)) {
            if(pos || length<header+8 || (length-header)%4)return false;
            for(size_t i=header;i<length;i+=4) {
                if(i==header+4)continue;
                if(!read_at(f,pos+i,h,4))return false;
                if(!memcmp(h,"heic",4)||!memcmp(h,"heix",4)||!memcmp(h,"hevc",4)||!memcmp(h,"hevx",4)||!memcmp(h,"mif1",4)||!memcmp(h,"msf1",4))brand=true;
            }
        } else if(!memcmp(h+4,"meta",4))meta=length>header+4;
        else if(!memcmp(h+4,"mdat",4))data=length>header;
        pos+=(size_t)length;
    }
    return brand && meta && data;
}
static bool saved_file(const char* path,folio_upload_kind_t kind) {
    FILE* f=fopen(path,"rb");if(!f)return false;
    unsigned char h[80]={0};bool ok=fseek(f,0,SEEK_END)==0;long length=ok?ftell(f):-1;
    size_t size=length>0?(size_t)length:0;
    ok=size && read_at(f,0,h,size<sizeof(h)?size:sizeof(h));
    if(ok)switch(kind) {
        case FOLIO_UPLOAD_PNG:ok=png_file(f,size);break;
        case FOLIO_UPLOAD_JPEG:ok=jpeg_file(f,size);break;
        case FOLIO_UPLOAD_GIF:ok=gif_file(f,size,h);break;
        case FOLIO_UPLOAD_WEBP:ok=webp_file(f,size,h);break;
        case FOLIO_UPLOAD_HEIC:case FOLIO_UPLOAD_HEIF:ok=heif_file(f,size);break;
        case FOLIO_UPLOAD_BMP: {
            uint32_t dib=le32(h+14),offset=le32(h+10);
            ok=size>=26 && !memcmp(h,"BM",2) && le32(h+2)==size && offset<size && dib<=size-14 && offset>=14u+dib;
            if(ok && dib==12)ok=le16(h+18) && le16(h+20) && le16(h+22)==1 && le16(h+24)>0;
            else if(ok && dib>=40)ok=size>=54 && le32(h+18) && le32(h+22) && le16(h+26)==1 && le16(h+28)>0;
            else ok=false;
            break;
        }
        case FOLIO_UPLOAD_PDF: {
            unsigned char tail[1025];size_t n=size<1024?size:1024;
            ok=size>=20 && !memcmp(h,"%PDF-",5) && h[5]>='1' && h[5]<='2' && h[6]=='.' && h[7]>='0' && h[7]<='9' && read_at(f,size-n,tail,n);
            if(ok){tail[n]=0;while(n && (tail[n-1]=='\r'||tail[n-1]=='\n'||tail[n-1]==' '||tail[n-1]=='\t'))--n;ok=n>=5 && !memcmp(tail+n-5,"%%EOF",5);}
            break;
        }
        case FOLIO_UPLOAD_MOBI:case FOLIO_UPLOAD_AZW3: {
            unsigned records=((unsigned)h[76]<<8)|h[77];uint32_t previous=0;unsigned char record[8]={0};
            ok=size>=94 && !memcmp(h+60,"BOOKMOBI",8) && records && 78u+(uint64_t)records*8u<=size;
            for(unsigned i=0;ok && i<records;++i) {
                ok=read_at(f,78u+i*8u,record,8);uint32_t offset=be32(record);
                ok=ok && offset>=78u+records*8u && offset<size && (!i || offset>previous);
                if(ok && !i){unsigned char mobi[24];ok=size-offset>=24 && read_at(f,offset,mobi,24) && !memcmp(mobi+16,"MOBI",4) && be32(mobi+20)>=24 && be32(mobi+20)<=size-offset-16;}
                previous=offset;
            }
            break;
        }
        default:ok=false;break;
    }
    if(ferror(f) || cancelled())ok=false;
    if(fclose(f)!=0)ok=false;
    return ok;
}

static unsigned unicode_bom(const unsigned char h[4],bool* little) {
    *little=h[0]==0xff;
    if(!memcmp(h,"\xff\xfe\0\0",4)){*little=true;return 4;}
    if(!memcmp(h,"\0\0\xfe\xff",4)){*little=false;return 4;}
    if((h[0]==0xff && h[1]==0xfe) || (h[0]==0xfe && h[1]==0xff))return 2;
    return 0;
}
bool folio_transfer_saved_only(const char* path,folio_upload_kind_t kind) {
    if(folio_upload_saved_only(kind))return true;
    if(kind!=FOLIO_UPLOAD_TXT)return false;
    unsigned char h[4]={0};bool little;FILE* f=fopen(path,"rb");
    if(!f)return true;
    size_t n=fread(h,1,4,f);
    fclose(f);
    return n>=2 && unicode_bom(h,&little)!=0;
}
static bool unicode_text(FILE* f,unsigned width,bool little) {
    unsigned char h[4];bool visible=false;uint32_t high=0;size_t count=0,n;
    if(fseek(f,(long)width,SEEK_SET)!=0)return false;
    while((n=fread(h,1,width,f))) {
        if(n!=width || cancelled())return false;
        uint32_t cp=width==4?(little?le32(h):be32(h)):(little?le16(h):((uint32_t)h[0]<<8)|h[1]);
        if(width==2 && high){if(cp<0xdc00 || cp>0xdfff)return false;cp=0x10000+((high-0xd800)<<10)+(cp-0xdc00);high=0;}
        else if(width==2 && cp>=0xd800 && cp<=0xdbff){high=cp;continue;}
        if(cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) || !cp || (cp<32 && cp!='\n' && cp!='\r' && cp!='\t' && cp!='\f'))return false;
        if(cp>32 && cp!=0xfeff && cp!=0x3000)visible=true;
        if(!(++count&2047))folio_epub_yield();
    }
    return !high && visible && !ferror(f);
}
bool folio_transfer_validate(const char* path,folio_upload_kind_t kind,char* error,size_t cap) {
    if(error && cap)error[0]=0;
    if(cancelled())return false;
    bool ok=false;
    if(kind==FOLIO_UPLOAD_EPUB)ok=validate_epub(path,error,cap);
    else if(kind==FOLIO_UPLOAD_TTF)ok=folio_font_validate_file(path);
    else if(kind==FOLIO_UPLOAD_PDF || folio_upload_saved_only(kind))ok=saved_file(path,kind);
    else if(kind==FOLIO_UPLOAD_FIRMWARE)ok=folio_update_validate_image(path,error,cap);
    else if(kind==FOLIO_UPLOAD_SHA256) {
        FILE* file=fopen(path,"rb");char bytes[162]={0};size_t length=file?fread(bytes,1,161,file):0;
        ok=file && length>=64 && length<=160 && !ferror(file);
        if(file && fclose(file)!=0)ok=false;
        for(size_t i=0;ok && i<64;++i)if(!((bytes[i]>='0'&&bytes[i]<='9')||(bytes[i]>='a'&&bytes[i]<='f')||(bytes[i]>='A'&&bytes[i]<='F')))ok=false;
        size_t pos=64;
        if(pos<length && bytes[pos]!=' ' && bytes[pos]!='\t' && bytes[pos]!='\r' && bytes[pos]!='\n')ok=false;
        while(pos<length && (bytes[pos]==' '||bytes[pos]=='\t'))++pos;
        if(pos<length && bytes[pos]=='*')++pos;
        if(pos<length && bytes[pos]!='\r' && bytes[pos]!='\n') {
            if(length-pos<9 || memcmp(bytes+pos,"folio.bin",9))ok=false;else pos+=9;
        }
        if(pos<length && bytes[pos]=='\r')++pos;
        if(pos<length && bytes[pos]=='\n')++pos;
        if(pos!=length)ok=false;
    } else if(kind==FOLIO_UPLOAD_TXT) {
        FILE* file=fopen(path,"rb");folio_encoding_t encoding;long length=0;
        unsigned char h[4]={0};bool little=false;unsigned width=0;
        if(file && fread(h,1,4,file)>=2)width=unicode_bom(h,&little);
        if(width){ok=unicode_text(file,width,little);if(fclose(file)!=0)ok=false;file=NULL;}
        else ok=file && folio_text_detect(file,&encoding,&length) && length>0;
        bool visible=width!=0;uint32_t cp;size_t count=0;
        while(ok && !width && folio_text_next(file,encoding,&cp)) {
            if(!(count++ & 2047)){if(cancelled()){ok=false;break;}folio_epub_yield();}
            if(cp==0xfffd) {
                // U+FFFD itself is valid text; a decoder replacement for invalid
                // source bytes is not. Inspect only the three consumed bytes.
                long end=ftell(file);unsigned char raw[3];
                ok=encoding==FOLIO_ENCODING_UTF8 && end>=3 && read_at(file,(size_t)end-3,raw,3) && !memcmp(raw,"\xef\xbf\xbd",3);
            }
            if(cp==0 || (cp<32 && cp!='\n' && cp!='\r' && cp!='\t' && cp!='\f'))ok=false;
            if(cp>32 && cp!=0xfeff && cp!=0x3000)visible=true;
        }
        if(file){if(ferror(file))ok=false;if(fclose(file)!=0)ok=false;}ok=ok && visible;
    }
    if(cancelled())ok=false;
    if(!ok && kind==FOLIO_UPLOAD_EPUB && error && cap && !error[0])
        snprintf(error,cap,"EPUB 容器损坏、超过安全资源上限或校验中断。");
    if(!ok && error && cap && !error[0])snprintf(error,cap,"文件结构损坏、类型不符或校验中断。");
    return ok;
}
