#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/folio_debug_protocol.h"
int main(void) {
    folio_debug_command_t c;
    assert(folio_debug_parse("@FOLIO 8 TAP 683 1215",&c)&&c.kind==DBG_TAP&&c.x==683&&c.y==1215);
    assert(folio_debug_parse("@FOLIO 1 STATUS",&c)&&c.kind==DBG_STATUS);
    assert(folio_debug_parse("@FOLIO 9 KEY 2",&c)&&c.kind==DBG_KEY);
    const char* bad[]={"","STATUS","@FOLIO -1 STATUS","@FOLIO 4294967296 STATUS","@FOLIO 1 KEY 1 junk",
        "@FOLIO 1 TAP 0 1216","@FOLIO 1 TAP 684 0","@FOLIO 1 TAP -1 0","@FOLIO 1 STATUS junk",
        "@FOLIO 1 TAP 0 0 1","@FOLIO 1 KEY 99999999999999999999999","@FOLIO 1 FORMAT"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!folio_debug_parse(bad[i],&c));
    uint8_t source[415872],restored[415872],chunk[256];
    for(unsigned mode=0;mode<3;++mode){
        for(size_t i=0;i<sizeof(source);++i)source[i]=mode==0?255:mode==1?(uint8_t)i:(uint8_t)(i/77);
        size_t cursor=0,written=0;
        while(cursor<sizeof(source)){
            size_t n=folio_debug_rle(source,sizeof(source),&cursor,chunk,sizeof(chunk));assert(n && !(n&1));
            for(size_t j=0;j<n;j+=2){assert(written+chunk[j]<=sizeof(restored));memset(restored+written,chunk[j+1],chunk[j]);written+=chunk[j];}
        }
        assert(written==sizeof(source)&&!memcmp(source,restored,sizeof(source)));
        assert(folio_debug_hash(source,sizeof(source))==folio_debug_hash(restored,sizeof(restored)));
    }
    puts("USB protocol: strict command bounds and full-frame chunked RLE roundtrips passed");
}
