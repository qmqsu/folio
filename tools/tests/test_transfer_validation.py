#!/usr/bin/env python3
"""Real parsers + atomic upload, under ASan/UBSan; no radio/hardware/network."""
from pathlib import Path
import os,subprocess,tempfile,zipfile,sys,struct,random,base64
from test_epub_integration import fixture_font
from test_cover import png
ROOT=Path(__file__).resolve().parents[2]
def main():
    with tempfile.TemporaryDirectory(prefix='folio-transfer-validation-') as tmp:
        folder=Path(tmp); fixtures=folder/'fixtures';fixtures.mkdir();card=folder/'card'
        data={'good.txt':'山间日记\nA quiet page.\n'.encode(),'gbk.txt':'中文阅读'.encode('gbk'),'binary.txt':b'PK\x03\x04\x00\x00binary','broken-utf8.txt':b'\xef\xbb\xbftext\xff','utf16.txt':b'\xff\xfeh\x00i\x00','blank.txt':b' \r\n\t','replacement.txt':'合法替换字符 �\n'.encode(),
              'good.ttf':fixture_font(),'good.png':png(100,160),'good.jpg':(ROOT/'tools/tests/fixtures/cover/gradient-800x1200.jpg').read_bytes(),
              'good.sha256':b'0123456789abcdef'*4+b'  folio.bin\n','bad.sha256':b'g'*64,'short.sha256':b'a'*63,'foreign.sha256':b'a'*64+b'  other.bin\n','nul.sha256':b'a'*64+b'\0'}
        data['broken.ttf']=data['good.ttf'][:100];data['broken.png']=data['good.png'][:-15];data['broken.jpg']=data['good.jpg'][:100];data['bad.epub']=b'not a zip file'
        data['gray.jpg']=(ROOT/'tools/tests/fixtures/cover/cancel-gray.jpg').read_bytes()
        data['progressive.jpg']=(ROOT/'tools/tests/fixtures/cover/cancel-progressive.jpg').read_bytes()
        data['utf16be.txt']=b'\xfe\xff'+'中文和emoji 📚'.encode('utf-16be')
        data['utf32le.txt']=b'\xff\xfe\x00\x00'+'中文书'.encode('utf-32le')
        data['utf32be.txt']=b'\x00\x00\xfe\xff'+'中文书'.encode('utf-32be')
        data['bad-surrogate.txt']=b'\xff\xfe\x00\xd8A\x00'
        data['bad-utf32.txt']=b'\xff\xfe\x00\x00\x00\x00\x11\x00'
        data['odd-utf16.txt']=b'\xff\xfeA\x00B'
        data['large-pixels.png']=png(2000,1500,0)
        data['large-bytes.png']=png(1600,1600,0,raw=b''.join(b'\x00'+random.Random(row).randbytes(1600) for row in range(1600)))
        data['depth16.png']=png(2,2,0,depth=16,raw=(b'\x00'+b'\x7f\xff'*2)*2)
        data['interlaced.png']=png(1,1,0,interlace=1,raw=b'\x00\x7f')
        data['large.jpg']=data['good.jpg'][:2]+(b'\xff\xe2\xff\xff'+b'x'*65533)*34+data['good.jpg'][2:]
        data['bad-crc.png']=data['good.png'][:-1]+bytes([data['good.png'][-1]^1])
        data['no-eoi.jpg']=data['good.jpg'][:-2]
        data['good.pdf']=b'%PDF-1.4\n1 0 obj<</Type/Catalog>>endobj\nxref\n0 2\n0000000000 65535 f \n0000000009 00000 n \ntrailer<</Root 1 0 R/Size 2>>\nstartxref\n44\n%%EOF\n'
        data['bad.pdf']=data['good.pdf'][:-9]
        data['good.gif']=base64.b64decode('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7')
        data['bad.gif']=data['good.gif'][:-1]
        data['good.bmp']=b'BM'+struct.pack('<IHHI',58,0,0,54)+struct.pack('<IiiHHIIiiII',40,1,1,1,24,0,4,2835,2835,0,0)+b'\x10\x20\x30\x00'
        data['good.webp']=base64.b64decode('UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEADsD+JaQAA3AAAAAA')
        def box(kind,payload):return struct.pack('>I',len(payload)+8)+kind+payload
        # Structural fixtures only; not a HEVC decode/readability claim.
        data['good.heic']=box(b'ftyp',b'heic\0\0\0\0mif1heic')+box(b'meta',b'\0\0\0\0'+box(b'free',b'\0'))+box(b'mdat',b'\x01\x02')
        data['bad.heic']=data['good.heic'][:-1]
        mobi=bytearray(86+16+232);mobi[:4]=b'Book';mobi[60:68]=b'BOOKMOBI';mobi[76:78]=struct.pack('>H',1);mobi[78:82]=struct.pack('>I',86)
        mobi[86:88]=b'\0\x01';mobi[102:106]=b'MOBI';mobi[106:110]=struct.pack('>I',232);data['good.mobi']=bytes(mobi)
        data['bad.mobi']=data['good.mobi'][:105]
        for name,content in data.items():(fixtures/name).write_bytes(content)
        with zipfile.ZipFile(fixtures/'good.epub','w',compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr('mimetype','application/epub+zip',compress_type=zipfile.ZIP_STORED)
            z.writestr('META-INF/container.xml','<container><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
            z.writestr('book.opf','<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>传书测试</dc:title></metadata><manifest><item id="c" href="c.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="c"/></spine></package>')
            z.writestr('c.xhtml','<html><body><p>真实正文 Reading.</p></body></html>')
        with zipfile.ZipFile(fixtures/'good.epub') as source:
            for target in ('long-chapter.epub','traversal.epub','crc.epub'):
                with zipfile.ZipFile(fixtures/target,'w',compression=zipfile.ZIP_DEFLATED) as z:
                    for entry in source.infolist():
                        value=source.read(entry.filename)
                        if target=='long-chapter.epub' and entry.filename=='c.xhtml':value=b'<html><body><p>'+b'book '*140000+b'</p></body></html>'
                        z.writestr(entry.filename,value)
                    if target=='traversal.epub':z.writestr('../outside.txt','bad')
        corrupt=bytearray((fixtures/'crc.epub').read_bytes());corrupt[40]^=1;(fixtures/'crc.epub').write_bytes(corrupt)
        subprocess.run([sys.executable,str(ROOT/'tools/tests/generate_update_fixtures.py'),str(fixtures)],check=True)
        (fixtures/'bad.bin').write_bytes((fixtures/'good.bin').read_bytes()[:-1])
        (fixtures/'broken.epub').write_bytes((fixtures/'good.epub').read_bytes()[:-10])
        toolchain=Path('/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin');sdk=Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
        compiler=os.environ.get('CC',str(toolchain/'clang') if (toolchain/'clang').exists() else 'cc')
        flags=['-isysroot',str(sdk),'-B'+str(toolchain)] if sdk.exists() else []
        sources=['tools/tests/test_transfer_validation.c','main/net/folio_upload.c','main/net/folio_transfer_validate.c','main/folio_update.c']+['main/book/'+f for f in ['folio_font.c','folio_text.c','folio_gbk_table.c','folio_epub.c','folio_zip.c','folio_xml.c','folio_cover.c']]
        cmd=[compiler,'-std=c11','-D_POSIX_C_SOURCE=200809L','-DFOLIO_EPUB_HOST_TEST',f'-DFOLIO_SD_ROOT="{card}"','-Wall','-Wextra','-Werror','-g','-O1','-fno-omit-frame-pointer','-fsanitize=address,undefined','-fno-sanitize-recover=all',*flags,'-Itools/tests/cover_stubs','-Itools/tests/epub_integration_stubs','-Itools/tests/update_stubs','-Itools/tests/ui_stubs','-Imain','-Imain/book','-Imain/net',*sources,'-lz','-lm','-o',str(folder/'test')]
        subprocess.run(cmd,cwd=ROOT,check=True);subprocess.run([str(folder/'test'),str(card),str(fixtures)],cwd=ROOT,check=True,timeout=60)
if __name__=='__main__':main()
