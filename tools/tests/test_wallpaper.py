#!/usr/bin/env python3
"""Generate original large fixtures and test actual C backends under ASan/UBSan."""
import binascii
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib
from PIL import Image
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'artifacts/firmware-0831/wallpaper'

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', binascii.crc32(kind + data) & 0xffffffff)

def png(w, h, *, raw=None, depth=8, interlace=0, extra=b''):
    if raw is None: raw = (b'\0' + bytes([127]) * w) * h
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w,h,depth,0,0,0,interlace)) + extra + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND',b'')

def fixtures():
    OUT.mkdir(parents=True, exist_ok=True)
    f = OUT / 'fixtures'; f.mkdir(exist_ok=True)
    small=Image.fromarray(np.tile(np.arange(120,dtype=np.uint8)*2,(80,1)))
    small.save(f/'small.jpg',quality=90);small.save(f/'small.png')
    small.save(f/'progressive.jpg',quality=90,progressive=True)
    small.convert('CMYK').save(f/'cmyk.jpg',quality=90)
    small.save(f/'gray.jpg',quality=90)
    small.convert('RGB').save(f/'restart.jpg',quality=90,restart_marker_blocks=1)
    Image.new('L',(1,7),85).save(f/'tiny.jpg',quality=100)
    Image.new('L',(8000,4000),127).save(f/'max-pixels.png')
    Image.new('RGBA',(8192,128),(85,85,85,255)).save(f/'wide-rgba.png')
    Image.new('L',(8191,200),85).save(f/'wide.jpg',quality=95)
    Image.new('L',(8192,1),85).save(f/'max-width.png')
    Image.fromarray(np.pad(np.zeros((19,16),dtype=np.uint8),((0,0),(0,1)),constant_values=255)).save(f/'odd.png')
    Image.open(f/'odd.png').save(f/'odd.jpg',quality=100)
    rgba=Image.new('RGBA',(128,128),(0,0,0,0));rgba.save(f/'rgba.png')
    small.convert('P').save(f/'palette.png')
    Image.new('RGB',(20,20)).save(f/'picture.bmp')
    noise=np.random.default_rng(83).integers(0,256,(6000,4000),dtype=np.uint8)
    Image.fromarray(noise).save(f/'large.png',compress_level=1)
    Image.fromarray(noise).save(f/'large-gray.jpg',quality=92)
    rgb=np.stack((noise[:2000,:3000],np.roll(noise[:2000,:3000],1,axis=0),np.roll(noise[:2000,:3000],2,axis=1)),axis=2)
    Image.fromarray(rgb).save(f/'large.jpg',quality=94,subsampling=0)
    Image.fromarray(rgb).save(f/'large-progressive.jpg',quality=90,progressive=True)
    del noise,rgb
    stripes=np.tile(np.array([0,255],dtype=np.uint8), (1216,342))
    Image.fromarray(stripes).save(f/'stripes.png')
    quadrants=np.zeros((80,120),dtype=np.uint8)
    quadrants[:40,60:]=85;quadrants[40:,:60]=170;quadrants[40:,60:]=255
    for orientation in range(1,9):
        exif=Image.Exif();exif[274]=orientation
        Image.fromarray(quadrants).save(f/f'exif-{orientation}.jpg',quality=100,exif=exif)
    compressed=zlib.compress((b'\0'+b'\x00\x55\xaa\xff'*128)*128)
    split=(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',128,128,8,6,0,0,0))+
           b''.join(chunk(b'IDAT',bytes([b])) for b in compressed)+chunk(b'IEND',b''))
    (f/'split-idat.png').write_bytes(split)
    oriented=(f/'exif-6.jpg').read_bytes(); app=oriented.index(b'\xff\xe1'); appsize=int.from_bytes(oriented[app+2:app+4],'big')+2
    exif=b'Exif\0\0II\x2a\0\x08\0\0\0\x01\0\x12\x01\x03\0\x01\0\0\0\x06\0\0\0\0\0\0\0'
    (f/'exif-little.jpg').write_bytes(oriented[:app]+b'\xff\xe1'+struct.pack('>H',len(exif)+2)+exif+oriented[app+appsize:])
    base=png(2,2)
    filler=b'Comment\0'+b'x'*(32*1024*1024-len(base)-12-8)
    exact=base[:33]+chunk(b'tEXt',filler)+base[33:]
    assert len(exact)==32*1024*1024
    (f/'max-bytes.png').write_bytes(exact)
    (f/'too-large.png').write_bytes(exact+b'x')
    del exact,filler
    jpg=(f/'small.jpg').read_bytes()
    wanted=32*1024*1024-len(jpg);segments=[]
    while wanted:
        n=min(wanted,65537)
        if 0<wanted-n<4:n-=4-(wanted-n)
        assert n>=4
        segments.append(b'\xff\xfe'+struct.pack('>H',n-2)+b'x'*(n-4));wanted-=n
    (f/'max-bytes.jpg').write_bytes(jpg[:2]+b''.join(segments)+jpg[2:]);del segments
    p=(f/'small.png').read_bytes()
    bad={
        'truncated.png':p[:-5], 'tail.png':p+b'garbage',
        'bad-crc.png':p[:-1]+bytes([p[-1]^1]),
        'excess.png':png(2,2,raw=(b'\0xx')*3), 'short.png':png(2,2,raw=b'\0xx'),
        'too-many-pixels.png':png(8000,4001,raw=b'\0'), 'too-wide.png':png(8193,1,raw=b'\0'),
        'interlaced.png':png(2,2,interlace=1), 'depth16.png':png(2,2,depth=16),
        'animation.png':png(2,2,extra=chunk(b'acTL',struct.pack('>II',1,0))),
        'truncated.jpg':jpg[:len(jpg)//2], 'missing-eoi.jpg':jpg[:-2],
        'extra-entropy.jpg':jpg[:-2]+b'\x12\x34\x56'+jpg[-2:],
        'no-scan.jpg':jpg[:jpg.index(b'\xff\xda')]+b'\xff\xd9',
    }
    sof=jpg.index(b'\xff\xc0');size=int.from_bytes(jpg[sof+2:sof+4],'big')+2
    bad['two-sof.jpg']=jpg[:sof]+jpg[sof:sof+size]+jpg[sof:]
    bad['bad-exif.jpg']=jpg[:2]+b'\xff\xe1\x00\x10Exif\0\0II\x2a\0\xff\xff\xff\x7f'+jpg[2:]
    dht=jpg.index(b'\xff\xc4');mut=bytearray(jpg);mut[dht+5]=255;bad['bad-dht.jpg']=bytes(mut)
    r=(f/'restart.jpg').read_bytes();marker=next((i for i in range(len(r)-1) if r[i]==255 and 0xD0<=r[i+1]<=0xD7),None)
    assert marker is not None
    bad['bad-restart.jpg']=r[:marker]+b'\xff\xd7'+r[marker+2:]
    for name,data in bad.items():(f/name).write_bytes(data)
    print('fixture bytes:', {n:(f/n).stat().st_size for n in ['large.png','large.jpg','large-gray.jpg','max-bytes.png','max-bytes.jpg']},flush=True)
    return f

def main():
    f=OUT/'fixtures' if '--reuse' in sys.argv else fixtures()
    toolchain=Path('/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin')
    sdk=Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    cc=os.environ.get('CC',str(toolchain/'clang') if (toolchain/'clang').exists() else 'cc')
    flags=['-isysroot',str(sdk),'-B'+str(toolchain)] if sdk.exists() else []
    for suffix,defines in [('',[]),('-fatfs',['-DFOLIO_WALLPAPER_TEST_FATFS=1'])]:
        binary=OUT/('test-wallpaper'+suffix)
        command=[cc,'-std=c11','-D_POSIX_C_SOURCE=200809L',*defines,'-g','-O1','-fno-omit-frame-pointer','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wall','-Wextra','-Werror',*flags,'-Itools/tests/cover_stubs','-Imain','-Imain/book','tools/tests/test_wallpaper.c','main/book/folio_cover.c','main/book/folio_jpeg.c','-lm','-o',str(binary)]
        subprocess.run(command,cwd=ROOT,check=True)
        subprocess.run([str(binary),str(f)],cwd=ROOT,check=True)
if __name__=='__main__':main()
