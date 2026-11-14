#!/usr/bin/env python3
"""Actual service/client/cache/ledger integration, no accounts and no network.

Uses synchronous helper calls; deliberately does not claim to test FreeRTOS
scheduling, physical TLS, display timing, or media power-loss guarantees.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import zipfile
import xml.etree.ElementTree as ET
from test_weread_client import encode_shards

ROOT = Path(__file__).resolve().parents[2]

def main():
    cc = os.environ.get('CC', '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang')
    sdk = '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    with tempfile.TemporaryDirectory(prefix='wr-svc-') as temp:
        folder = Path(temp).resolve()
        common = ['-isysroot',sdk,'-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wno-deprecated-declarations',f'-DFOLIO_SD_ROOT="{folder / "card"}"','-DFOLIO_EPUB_HOST_TEST','-DFOLIO_WEREAD_HOST_TEST','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'main'),'-I',str(ROOT/'main/book'),'-I',str(ROOT/'main/net'),'-I',str(ROOT/'tools/tests/weread_service_stubs'),'-I',str(ROOT/'tools/tests/weread_transport_stubs')]
        objects=[]
        for index, name in enumerate(['main/folio_storage.c','main/book/folio_weread_cache.c','main/net/folio_weread_time.c','main/book/folio_epub.c','main/book/folio_zip.c','main/book/folio_xml.c','main/net/weread/vendor/cjson/cJSON.c']):
            target=folder/f'{index}.o'; flags=['-Drename=folio_test_time_rename'] if name.endswith('folio_weread_time.c') else []
            subprocess.run([cc,*common,'-std=c11',*flags,'-c',str(ROOT/name),'-o',str(target)],check=True);objects.append(str(target))
        binary=folder/'runner'
        subprocess.run([cc+'++',*common,'-std=c++17','-Wno-missing-field-initializers',str(ROOT/'tools/tests/weread_service_runner.cc'),str(ROOT/'main/net/weread/folio_weread_client.cc'),str(ROOT/'main/net/weread/folio_weread_crypto.cc'),str(ROOT/'main/net/weread/folio_weread_session.cc'),*objects,'-pthread','-lz','-o',str(binary)],check=True)
        for i in range(4):
            raw=f'<html><body><p>ABCDEFGHIJKLMN Chapter {i} 中文😀 &amp; 正文</p>\n<p>第二段</p></body></html>'.encode()
            (folder/f'raw{i}').write_bytes(raw)
            for part, data in enumerate(encode_shards(raw,3)):(folder/f'c{i}_{part}').write_bytes(data)
        subprocess.run([str(binary),str(folder)],check=True,timeout=60)
        exports=list((folder/'card/books').glob('*.epub'))+[folder/'with-cover.epub']
        assert len(exports)>=2
        for epub in exports:
            with zipfile.ZipFile(epub) as z:
                assert z.infolist()[0].filename=='mimetype'
                assert z.infolist()[0].compress_type==zipfile.ZIP_STORED
                assert z.read('mimetype')==b'application/epub+zip' and z.testzip() is None
                for name in z.namelist():
                    if name.endswith(('.xml','.opf','.ncx','.xhtml')):ET.fromstring(z.read(name))
                opf=ET.fromstring(z.read('OEBPS/content.opf'))
                ns={'o':'http://www.idpf.org/2007/opf'}
                manifest={i.attrib['id']:i.attrib['href'] for i in opf.findall('o:manifest/o:item',ns)}
                for href in manifest.values():assert 'OEBPS/'+href in z.namelist()
                for item in opf.findall('o:spine/o:itemref',ns):assert item.attrib['idref'] in manifest
                if epub.name=='with-cover.epub':
                    assert z.read('OEBPS/cover.png')==(ROOT/'tools/tests/fixtures/ui-cover-light.png').read_bytes()
                    assert opf.find('o:spine/o:itemref',ns).attrib['idref']=='cover'
        assert not list((folder/'card/books').glob('*.wrb*'))
        print('Independent EPUB ZIP CRC/XML/manifest/spine/embedded-cover validation passed', flush=True)
        # Reuse a real exported EPUB, but run in a new process on an empty card.
        card, downloaded = folder/'card', folder/'downloaded-card'
        assert card.resolve().parent == folder and not downloaded.exists()
        card.rename(downloaded)
        subprocess.run([str(binary),str(folder),'--standalone'],check=True,timeout=60)

if __name__=='__main__':main()
