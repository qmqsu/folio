#!/usr/bin/env python3
"""Test-only signed fixtures. Known scalar 12345 is NEVER a release key."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import runpy
import sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
image_builder = runpy.run_path(str(Path(__file__).with_name('generate_update_fixtures.py')))['image']
key = ec.derive_private_key(12345, ec.SECP256R1())
public = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
(root / 'test_release_key.h').write_text('#pragma once\n#include <stdint.h>\n#define FOLIO_RELEASE_KEY_ID "folio-release-2026-3"\nstatic const uint8_t folio_release_public_key[65] = {' + ','.join(hex(b) for b in public) + '};\n')
version = '0.8.32-dev.1'
image = image_builder(version)
(root / 'online.bin').write_bytes(image)
(root / 'different.bin').write_bytes(image_builder('0.8.32-dev.2'))
(root / 'bad-chip.bin').write_bytes(image[:12] + b'\x05' + image[13:])
base = dict(schema=1, kind='ota', project='Folio', chip='esp32s3', hardware='RDP-G01-W',
            layout='folio-16m-factory-storage-ota-v2', channel='preview', version=version,
            sequence=8032001, image=dict(url=f'https://qmqsu.github.io/folio/firmware/{version}/folio.bin',
            size=len(image), sha256=hashlib.sha256(image).hexdigest()), notes='真实签名更新\n安全验证')
cases = []
def raw_json(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(',', ':')).encode()
def envelope(payload, signer=key):
    if not isinstance(payload, bytes):
        payload = raw_json(payload)
    r, s = utils.decode_dss_signature(signer.sign(payload, ec.ECDSA(hashes.SHA256())))
    return dict(key_id='folio-release-2026-3', payload=base64.b64encode(payload).decode(),
                signature=base64.b64encode(r.to_bytes(32, 'big') + s.to_bytes(32, 'big')).decode())
def save(name, payload=None, *, env=None, raw=None, good=False):
    if raw is None:
        raw = raw_json(env if env is not None else envelope(base if payload is None else payload))
    (root / f'{name}.json').write_bytes(raw)
    cases.append((name, int(good)))
def changed(name, field, value, good=False):
    obj = copy.deepcopy(base)
    if field.startswith('image.'):
        obj['image'][field[6:]] = value
    else:
        obj[field] = value
    save(name, obj, good=good)

save('valid', good=True)
changed('maximum-sequence', 'sequence', 4294967295, True)
changed('equal-sequence', 'sequence', 8032000)
changed('older-sequence', 'sequence', 8031999)
changed('zero-sequence', 'sequence', 0)
changed('unknown-schema', 'schema', 2)
changed('wrong-kind', 'kind', 'usb')
changed('wrong-project', 'project', 'Other')
changed('wrong-chip', 'chip', 'esp32')
changed('wrong-hardware', 'hardware', 'RDP-G02')
changed('wrong-layout', 'layout', 'other-layout')
changed('wrong-channel', 'channel', 'stable')
changed('bad-channel', 'channel', 'beta')
changed('string-sequence', 'sequence', '8032001')
changed('bool-sequence', 'sequence', True)
changed('float-sequence', 'sequence', 8032001.0)
changed('negative-sequence', 'sequence', -1)
changed('overflow-sequence', 'sequence', 4294967296)
changed('null-version', 'version', None)
changed('oversize-image', 'image.size', 3145729)
changed('zero-size', 'image.size', 0)
changed('negative-size', 'image.size', -1)
changed('bool-size', 'image.size', True)
changed('float-size', 'image.size', 16720.0)
changed('uppercase-hash', 'image.sha256', 'F' * 64)
changed('short-hash', 'image.sha256', 'a' * 63)
changed('wrong-hash', 'image.sha256', 'a' * 64, True)  # valid signature; image must later fail
changed('long-version', 'version', 'a' * 32)
changed('nul-version', 'version', '0.8\x00evil')
changed('path-version', 'version', '../evil')
changed('slash-version', 'version', 'a/b')
changed('unicode-version', 'version', '版本')
for name, url in {
    'http': base['image']['url'].replace('https:', 'http:'),
    'foreign-host': base['image']['url'].replace('qmqsu.github.io', 'evil.example'),
    'userinfo': base['image']['url'].replace('qmqsu.github.io', 'qmqsu.github.io@evil.example'),
    'lookalike-host': base['image']['url'].replace('qmqsu.github.io', 'qmqsu.github.io.evil.example'),
    'port': base['image']['url'].replace('.io/', '.io:443/'),
    'url-query': base['image']['url'] + '?x=1', 'url-fragment': base['image']['url'] + '#x',
    'url-traversal': base['image']['url'].replace('/firmware/', '/firmware/../'),
    'url-encoded': base['image']['url'].replace('/firmware/', '/firmware/%2e%2e/'),
    'url-version': base['image']['url'].replace(version, '0.8.32-dev.2'),
    'backslash': base['image']['url'].replace('/firmware/', '\\firmware/'),
}.items():
    changed(name, 'image.url', url)
changed('notes-512', 'notes', '界' * 170 + 'ab', True)
changed('notes-513', 'notes', '界' * 171)
changed('notes-nul', 'notes', '\x00')
obj = copy.deepcopy(base); obj.pop('notes'); save('optional-notes', obj, good=True)
for field in base:
    if field != 'notes':
        obj = copy.deepcopy(base); obj.pop(field); save('missing-' + field, obj)
obj = copy.deepcopy(base); obj['unexpected'] = 1; save('unknown-field', obj)
obj = copy.deepcopy(base); obj['image']['extra'] = 1; save('unknown-image-field', obj)
for field in base:
    raw = raw_json(base)
    token = raw_json(field) + b':'
    at = raw.index(token) + len(token)
    value = raw_json(base[field])
    raw = raw[:at] + value + b',' + token + raw[at:]
    save('duplicate-' + field, raw)
for field in ('url', 'size', 'sha256'):
    raw = raw_json(base)
    token = raw_json(field) + b':'
    at = raw.index(token) + len(token)
    save('duplicate-image-' + field, raw[:at] + raw_json(base['image'][field]) + b',' + token + raw[at:])
raw = raw_json(base)
for name, data in {
    'exponent': raw.replace(b'8032001', b'8.032001e6'),
    'leading-zero': raw.replace(b'8032001', b'08032001'),
    'plus': raw.replace(b'8032001', b'+8032001'),
    'nan': raw.replace(b'8032001', b'NaN'),
    'infinity': raw.replace(b'8032001', b'Infinity'),
    'trailing-comma': raw[:-1] + b',}', 'trailing-bytes': raw + b'[]',
    'lone-surrogate': raw.replace('真实'.encode(), b'\\ud800'),
    'invalid-utf8': raw.replace('真实'.encode(), b'\xc0\xaf'),
    'escaped-duplicate': raw.replace(b'"schema":1', b'"schema":1,"sch\\u0065ma":1'),
    'deep-object': raw.replace(b'8032001', b'[' * 100 + b'1' + b']' * 100),
}.items():
    save(name, data)
obj = envelope(base); obj['key_id'] = 'other'; save('unknown-key', env=obj)
obj = envelope(base); obj['signature'] = base64.b64encode(bytes(64)).decode(); save('bad-signature', env=obj)
obj = envelope(base, ec.derive_private_key(54321, ec.SECP256R1())); save('foreign-key', env=obj)
obj = envelope(base); obj['payload'] = base64.b64encode(raw.replace(b'8032001', b'8032002')).decode(); save('tampered-payload', env=obj)
obj = envelope(base); obj['signature'] = obj['signature'][:-2] + 'B='; save('noncanonical-base64', env=obj)
obj = envelope(base); obj['signature'] = base64.b64encode(bytes(63)).decode(); save('short-signature', env=obj)
obj = envelope(base); obj['payload'] += '\n'; save('base64-whitespace', env=obj)
obj = envelope(base); obj['extra'] = 1; save('unknown-envelope-field', env=obj)
raw = raw_json(envelope(base))
for field in ('key_id', 'payload', 'signature'):
    token = raw_json(field) + b':'
    at = raw.index(token) + len(token)
    save('duplicate-envelope-' + field, raw=raw[:at] + raw_json(envelope(base)[field]) + b',' + token + raw[at:])
save('oversize-envelope', raw=raw + b' ' * 4096)
save('trailing-envelope', raw=raw + b'junk')
# Signed wrong-chip bytes are rejected by the actual image parser, not the hash.
obj = copy.deepcopy(base); obj['image']['sha256'] = hashlib.sha256((root/'bad-chip.bin').read_bytes()).hexdigest()
save('signed-bad-chip-image', obj, good=True)
# Signed version/data mismatch is rejected at the app descriptor boundary.
obj = copy.deepcopy(base); obj['image']['sha256'] = hashlib.sha256((root/'different.bin').read_bytes()).hexdigest()
save('signed-wrong-version-image', obj, good=True)
(root / 'cases.tsv').write_text(''.join(f'{good}\t{name}\n' for name, good in cases))
print(f'Generated {len(cases)} signed/hostile manifest fixtures with test-only P256 key')
