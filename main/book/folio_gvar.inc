// OpenType TrueType outline variations at the selected weight. All offsets are relative to
// a bounded glyph record. The default instance never needs variation data.
typedef struct { const uint8_t* p; const uint8_t* end; } var_stream_t;
typedef struct { int base[2]; float delta[2], sum[2]; bool touched; uint8_t flag; } var_point_t;

static bool var_take(var_stream_t* s, size_t n, const uint8_t** data) {
    if (n > (size_t)(s->end-s->p)) return false;
    *data=s->p; s->p+=n; return true;
}
static bool var_u8(var_stream_t* s, unsigned* value) {
    const uint8_t* b;
    if (!var_take(s,1,&b)) return false;
    *value=*b; return true;
}
static bool var_u16(var_stream_t* s, unsigned* value) {
    const uint8_t* b;
    if (!var_take(s,2,&b)) return false;
    *value=be16(b); return true;
}
static bool var_points(var_stream_t* s, unsigned limit, uint16_t* indices, unsigned* count) {
    unsigned n, low;
    if (!var_u8(s,&n)) return false;
    if (n & 128) {
        if (!var_u8(s,&low)) return false;
        n=((n&127)<<8)|low;
    }
    if (n>limit) return false;
    *count=n;
    unsigned pos=0, index=0;
    while (pos<n) {
        unsigned control;
        if (!var_u8(s,&control)) return false;
        unsigned run=(control&127)+1;
        if (run>n-pos) return false;
        while (run--) {
            unsigned d;
            if (!(control&128 ? var_u16(s,&d) : var_u8(s,&d))) return false;
            index+=d;
            if (index>=limit || (pos && !d)) return false;
            indices[pos++]=(uint16_t)index;
        }
    }
    return true;
}
static bool var_deltas(var_stream_t* s,var_point_t* points,unsigned n,const uint16_t* indices,unsigned axis) {
    unsigned pos=0;
    while (pos<n) {
        unsigned c;
        if (!var_u8(s,&c)) return false;
        unsigned run=(c&63)+1;
        if (run>n-pos) return false;
        while (run--) {
            int delta=0;
            if ((c & 192) == 192) {
                const uint8_t* raw;
                if (!var_take(s, 4, &raw)) return false;
                delta = (int32_t)be32(raw);
                if (delta < -65535 || delta > 65535) return false;
            } else if (!(c&128)) {
                unsigned raw;
                if (!(c&64 ? var_u16(s,&raw) : var_u8(s,&raw))) return false;
                delta=c&64 ? (int16_t)raw : (int8_t)raw;
            }
            unsigned i=indices ? indices[pos] : pos;
            points[i].delta[axis]=delta; points[i].touched=true; ++pos;
        }
    }
    return true;
}
static void var_interpolate(var_point_t* p,unsigned first,unsigned last) {
    unsigned start=first;
    while (start<=last && !p[start].touched) ++start;
    if (start>last) return;
    unsigned a=start;
    do {
        unsigned b=a;
        do { b=b==last ? first : b+1; } while (!p[b].touched);
        for (unsigned i=a;;) {
            i=i==last ? first : i+1;
            if (i==b) break;
            for (unsigned dim=0;dim<2;++dim) {
                unsigned lo=p[a].base[dim]<=p[b].base[dim] ? a : b;
                unsigned hi=lo==a ? b : a;
                int x=p[i].base[dim], x0=p[lo].base[dim], x1=p[hi].base[dim];
                float d0=p[lo].delta[dim], d1=p[hi].delta[dim];
                p[i].delta[dim]=x0==x1 ? (d0==d1?d0:0) :
                    x<=x0 ? d0 : x>=x1 ? d1 : d0+(d1-d0)*(x-x0)/(x1-x0);
            }
        }
        a=b;
    } while(a!=start);
}
static bool var_tuples(card_face_t* face,var_stream_t data,float coord,var_point_t* points,
                       unsigned count,const uint8_t* ends,unsigned contours) {
    const uint8_t* origin=data.p;
    unsigned flags,offset;
    if (!var_u16(&data,&flags) || !var_u16(&data,&offset) || offset<4 || offset>(size_t)(data.end-origin)) return false;
    if ((flags&4095)>128) return false;
    var_stream_t bodies={origin+offset,data.end};
    data.end=bodies.p;
    uint16_t* indexes=malloc(2u*count*sizeof(*indexes));
    if (!indexes) return false;
    unsigned shared=0;
    bool ok=!(flags&0x8000) || var_points(&bodies,count,indexes,&shared);
    for (unsigned t=0;ok && t<(flags&4095);++t) {
        unsigned size,tuple;
        const uint8_t* bytes;
        if (!var_u16(&data,&size) || !var_u16(&data,&tuple) || !var_take(&bodies,size,&bytes)) {ok=false;break;}
        const uint8_t *embedded=NULL,*lower=NULL,*upper=NULL;
        unsigned axes=face->axis_count,index=tuple&4095;
        if (tuple&0x8000) {
            if (!var_take(&data,2*axes,&embedded)) {ok=false;break;}
        } else if (index>=face->tuple_count) {ok=false;break;}
        if ((tuple&0x4000) && (!var_take(&data,2*axes,&lower) || !var_take(&data,2*axes,&upper))) {ok=false;break;}
        float scale=1;
        for(unsigned dim=0;dim<axes;++dim) {
            int peak=(int16_t)(embedded ? be16(embedded+2*dim) : face->tuple_peaks[index*axes+dim]);
            int lo=lower ? (int16_t)be16(lower+2*dim) : peak<0?peak:0;
            int hi=upper ? (int16_t)be16(upper+2*dim) : peak>0?peak:0;
            float x=dim==face->weight_axis ? coord : 0;
            if(!peak || lo>peak || peak>hi || (lo<0 && hi>0)) continue;
            if(x<lo || x>hi) {scale=0;break;}
            if(x!=peak) scale*=x<peak ? (x-lo)/(peak-lo) : (hi-x)/(hi-peak);
        }
        if (!scale) continue;
        var_stream_t deltas={bytes,bytes+size};
        unsigned n=shared; uint16_t* ids=indexes;
        if (tuple&0x2000) {
            ids=indexes+count;
            if (!var_points(&deltas,count,ids,&n)) {ok=false;break;}
        }
        for(unsigned i=0;i<count;++i) {points[i].touched=false;points[i].delta[0]=points[i].delta[1]=0;}
        for(unsigned d=0;d<2 && ok;++d) ok=var_deltas(&deltas,points,n?n:count,n?ids:NULL,d);
        if (!ok) break;
        if(n) for(unsigned j=0,first=0;j<contours;++j) {
            unsigned last=be16(ends+j*2);
            var_interpolate(points,first,last); first=last+1;
        }
        for(unsigned i=0;i<count;++i) for(unsigned d=0;d<2;++d) points[i].sum[d]+=scale*points[i].delta[d];
    }
    free(indexes);return ok;
}
static void var_store16(uint8_t* p,int value) {
    unsigned v=(uint16_t)value;p[0]=v>>8;p[1]=v;
}
// Called after the full simple glyph has passed the existing outline validator.
static bool var_simple(card_face_t* face,unsigned gid,int step,const uint8_t* g,unsigned contours,
                       uint8_t** result,size_t* out_size) {
    uint8_t offsets[8]; unsigned stride=face->wide_offsets ? 4 : 2;
    if (!card_read(face->file,face->var_offsets+gid*stride,offsets,2*stride)) return false;
    uint32_t start=face->wide_offsets ? be32(offsets) : 2u*be16(offsets);
    uint32_t end=face->wide_offsets ? be32(offsets+4) : 2u*be16(offsets+2);
    if (end<start || end>face->var_length || end-start>128*1024) return false;
    if (end==start || !contours) return true;
    unsigned n=1+be16(g+10+2*(contours-1));
    size_t header=12+2*contours;
    size_t length=header+5*n;
    var_point_t* points=calloc(n+4,sizeof(*points));
    uint8_t* record=malloc(end-start);
    uint8_t* out=malloc(length);
    bool ok=false;
    if (!points || !record || !out || !card_read(face->file,face->var_data+start,record,end-start)) goto done;
    const uint8_t* ptr=g+header+be16(g+header-2);
    for(unsigned i=0;i<n;) {
        unsigned f=*ptr++,run=f&8 ? 1u+*ptr++ : 1;
        for(unsigned j=0;j<run;++j) points[i++].flag=f;
    }
    for(unsigned d=0;d<2;++d) {
        int x=0;
        for(unsigned i=0;i<n;++i) {
            unsigned flag=points[i].flag,short_bit=2u<<d,same_bit=16u<<d;
            if (flag&short_bit) {int v=*ptr++;x+=(flag&same_bit)?v:-v;}
            else if (!(flag&same_bit)) {x+=(int16_t)be16(ptr);ptr+=2;}
            points[i].base[d]=x;
        }
    }
    if (!var_tuples(face,(var_stream_t){record,record+end-start},face->coords[step+2],points,n+4,g+10,contours)) goto done;
    memcpy(out,g,header);var_store16(out+header-2,0);
    for(unsigned i=0;i<n;++i) out[header+i]=points[i].flag&1;
    for(unsigned d=0;d<2;++d) {
        int min=32767,max=-32768,previous=0;
        for(unsigned i=0;i<n;++i) {
            float coord=points[i].base[d]+points[i].sum[d];
            if (coord < -32768 || coord > 32767) goto done;
            int x=(int)lroundf(coord),delta=x-previous;
            if (delta < -32768 || delta > 32767) goto done;
            var_store16(out+header+n+2*(n*d+i),delta);previous=x;
            if(x<min) min=x;
            if(x>max) max=x;
        }
        var_store16(out+2+2*d,min);var_store16(out+6+2*d,max);
    }
    *result=out;*out_size=length;out=NULL;ok=true;
done:
    free(out);free(points);free(record);return ok;
}
