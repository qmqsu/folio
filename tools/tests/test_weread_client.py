#!/usr/bin/env python3
"""No-account WeRead protocol tests: actual client/parser/crypto, injected transport."""
from pathlib import Path
import base64
import hashlib
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'main/net/weread'

def encode_shards(data: bytes, count: int) -> list[bytes]:
    encoded = bytearray(base64.urlsafe_b64encode(data).rstrip(b'='))
    length = len(encoded)
    if length < 4:
        positions = []
    elif length < 11:
        positions = [0, 2]
    else:
        tail = min(4, (length + 9) // 10)
        digits = ''.join(str(sum(1 << (bit * 2) for bit in range(8) if (v >> bit) & 1)) for v in encoded[-tail:][::-1])
        mod = length - tail - 2
        width = len(str(mod))
        positions = []
        for i in range(0, len(digits) - width, width):
            if len(positions) >= 10: break
            positions += [int(digits[i:i + width]) % mod, int(digits[i + 1:i + 1 + width]) % mod]
    for i in range(0, len(positions), 2):
        for d in range(2):
            a, b = positions[i] + d, positions[i + 1] + d
            encoded[a], encoded[b] = encoded[b], encoded[a]
    payload = b'1' + bytes(encoded)
    parts = [payload[i * len(payload) // count:(i + 1) * len(payload) // count] for i in range(count)]
    return [hashlib.md5(p).hexdigest().upper().encode() + p for p in parts]

def main():
    cc = os.environ.get('CC', '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang')
    sdk = '/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    flags = ['-isysroot', sdk, '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    with tempfile.TemporaryDirectory(prefix='folio-weread-client-') as tmp:
        path = Path(tmp)
        subprocess.run([cc, *flags, '-std=c11', '-Wno-deprecated-declarations', '-DCJSON_NESTING_LIMIT=32', '-c', str(SOURCE/'vendor/cjson/cJSON.c'), '-o', str(path/'json.o')], check=True)
        subprocess.run([cc+'++', *flags, '-std=c++17', '-Wno-deprecated-declarations', '-Wno-missing-field-initializers', '-DFOLIO_WEREAD_HOST_TEST', '-DFOLIO_WEREAD_TEST_CLOCK', '-I', str(SOURCE),
            str(ROOT/'tools/tests/weread_client_runner.cc'), *(str(SOURCE/n) for n in ['folio_weread_client.cc','folio_weread_crypto.cc','folio_weread_transport.cc']), str(path/'json.o'), '-o', str(path/'runner')], check=True)
        for n in (128, 129):
            (path/f'shelf{n}.json').write_text(json.dumps({'books':[{'bookId':f'b{i}','title':f'Title {i}'} for i in range(n)]}))
        for n in (512, 513):
            (path/f'chapters{n}.json').write_text(json.dumps({'data':[{'bookId':'book1','updated':[{'chapterUid':f'c{i}','chapterIdx':i,'title':f'Chapter {i}'} for i in range(n)]}]}))
        texts = {'xhtml':'<html><body><p id="a">中文😀 &amp; 文本</p>\n<p> untouched </p></body></html>', 'plain':'原始TXT😀\r\n second line\n末尾'}
        for name, data in texts.items():
            raw=data.encode(); (path/name).write_bytes(raw)
            for i, shard in enumerate(encode_shards(raw,3 if name=='xhtml' else 2)):
                (path/f'{"x" if name=="xhtml" else "t"}{i}').write_bytes(shard)
        # Independent Python ID encoding reference, for a nontrivial 9-digit input.
        val='123456789'; digest=hashlib.md5(val.encode()).hexdigest(); chunk=f'{int(val):x}'
        result=digest[:3]+'32'+digest[30:]+f'{len(chunk):02x}'+chunk
        result+=digest[:max(0,20-len(result))]
        (path/'id_expected').write_text(result+hashlib.md5(result.encode()).hexdigest()[:3])
        subprocess.run([str(path/'runner'),str(path)],check=True,timeout=30)
        # Compile the actual ESP transport branch against a tiny deterministic SDK surface.
        subprocess.run([cc+'++', *flags, '-std=c++17', '-Wno-deprecated-declarations', '-I', str(ROOT/'tools/tests/weread_transport_stubs'), '-I', str(SOURCE), '-c', str(SOURCE/'folio_weread_transport.cc'), '-o', str(path/'transport.o')], check=True)
        subprocess.run([cc+'++', *flags, '-std=c++17', '-Wno-deprecated-declarations', '-DFOLIO_WEREAD_HOST_TEST', '-I', str(SOURCE), '-I', str(ROOT/'tools/tests/weread_transport_stubs'), str(ROOT/'tools/tests/weread_transport_runner.cc'), str(SOURCE/'folio_weread_client.cc'), str(SOURCE/'folio_weread_crypto.cc'), str(path/'transport.o'), str(path/'json.o'), '-o', str(path/'transport-runner')], check=True)
        subprocess.run([str(path/'transport-runner')],check=True,timeout=30)

if __name__=='__main__': main()
