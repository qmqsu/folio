#!/usr/bin/env python3
"""Build/validate FOLFONT1: a 5MiB read-only font-resource image, never flash.

No dependency installation or device/USB I/O. Font asset integrity is checked
before writing. --verify-only validates an existing image independently of local
TTFs. --self-test runs deterministic corrupt-input rejection tests in memory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b'FOLFONT1'
CAPACITY = 5 * 1024 * 1024
HEADER_BYTES = 4096
ENTRY_BYTES = 128
ENTRY_OFFSET = 64
DATA_OFFSET = 49152
GUARD_BYTES = 65536
MAX_RAW_BYTES = 6 * 1024 * 1024
IDS = ('hei', 'kai', 'yuan', 'symbols')


class PackError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PackError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def crc(data):
    return zlib.crc32(data) & 0xffffffff


def align(value):
    return (value + 4095) & ~4095


def json_bytes(value, compact=False):
    if compact:
        return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode()
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode()


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate JSON key')
        result[key] = value
    return result


def load_json(raw):
    try:
        return json.loads(raw.decode('utf-8'), object_pairs_hook=unique_pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PackError('invalid UTF-8 JSON') from exc


def u32(raw, offset):
    return struct.unpack_from('<I', raw, offset)[0]


def put32(raw, offset, value):
    struct.pack_into('<I', raw, offset, value)


def header_crc(header):
    header = bytearray(header)
    put32(header, 44, 0)
    return crc(header)


def decompress_exact(packed, expected):
    require(type(expected) is int and 12 <= expected <= MAX_RAW_BYTES, 'raw length outside renderer cap')
    decoder = zlib.decompressobj(-15)
    try:
        raw = decoder.decompress(packed, expected + 1)
    except zlib.error as exc:
        raise PackError('invalid raw DEFLATE') from exc
    require(len(raw) == expected, 'uncompressed length mismatch')
    require(decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
            'DEFLATE missing end or has trailing bytes')
    return raw


def hex_field(value, digits, label):
    require(isinstance(value, str) and len(value) == digits and
            all(c in '0123456789abcdef' for c in value), 'invalid ' + label)
    return value


def verify_image(image):
    require(len(image) == CAPACITY, 'image must be exactly 5MiB')
    header = image[:HEADER_BYTES]
    require(header[:8] == MAGIC, 'bad magic')
    require(u32(header, 8) == 1 and u32(header, 12) == HEADER_BYTES, 'unsupported header version/size')
    total_used, count = u32(header, 16), u32(header, 20)
    require(count == 4 and u32(header, 24) == ENTRY_BYTES and u32(header, 28) == ENTRY_OFFSET,
            'invalid entry count/size/offset')
    require(DATA_OFFSET <= total_used <= CAPACITY - GUARD_BYTES and total_used % 4096 == 0,
            'total_used exceeds guard or is not aligned')
    meta_offset, meta_len = u32(header, 32), u32(header, 36)
    require(meta_offset == HEADER_BYTES and 0 < meta_len <= DATA_OFFSET - HEADER_BYTES,
            'metadata range invalid')
    require(header[48:64] == b'\0' * 16, 'header reserved fields nonzero')
    require(header[ENTRY_OFFSET + 4 * ENTRY_BYTES:] == b'\0' * (HEADER_BYTES - ENTRY_OFFSET - 4 * ENTRY_BYTES),
            'header padding nonzero')
    require(u32(header, 44) == header_crc(header), 'header CRC32 mismatch')
    meta_raw = image[meta_offset:meta_offset + meta_len]
    require(crc(meta_raw) == u32(header, 40), 'metadata CRC32 mismatch')
    require(image[meta_offset + meta_len:DATA_OFFSET] == b'\xff' * (DATA_OFFSET - meta_offset - meta_len),
            'metadata padding not erased')
    metadata = load_json(meta_raw)
    require(isinstance(metadata, dict) and set(metadata) == {'manifest', 'sources', 'notice'}, 'metadata root shape invalid')
    manifest, sources, notice = metadata['manifest'], metadata['sources'], metadata['notice']
    require(isinstance(manifest, dict) and manifest.get('schema') == 'folio-font-assets-v1', 'invalid font manifest')
    require(isinstance(sources, dict) and sources.get('schema') == 'folio-font-sources-v1', 'invalid source manifest')
    require(isinstance(notice, str), 'notice must be readable UTF-8 text')
    notice_record = manifest.get('notice', {})
    require(len(notice.encode()) == notice_record.get('bytes') and sha(notice.encode()) == notice_record.get('sha256'),
            'notice length/hash mismatch')
    rows = manifest.get('fonts')
    specs = sources.get('fonts')
    require(isinstance(rows, list) and len(rows) == 4 and all(isinstance(r, dict) for r in rows), 'font manifest count/shape invalid')
    require(isinstance(specs, list) and len(specs) == 4 and all(isinstance(r, dict) for r in specs), 'source manifest count/shape invalid')
    require(tuple(r.get('id') for r in rows) == IDS and tuple(r.get('id') for r in specs) == IDS, 'manifest font IDs/order invalid')
    coverage = manifest.get('coverage', {})
    require(coverage.get('A_preserved_count') == 4538 and coverage.get('B_count') == 7545 and
            coverage.get('hei_B_complete') is True and coverage.get('kai_B_complete') is True and
            coverage.get('yuan_plus_symbols_B_complete') is True, 'coverage contract shrank')
    require(rows[2].get('fallback_id') == 'symbols' and rows[3].get('selectable') is False,
            'Yuan fallback contract missing')
    entries, cursor = [], DATA_OFFSET
    for i, expected_id in enumerate(IDS):
        base = ENTRY_OFFSET + i * ENTRY_BYTES
        entry = header[base:base + ENTRY_BYTES]
        require(entry[:16] == expected_id.encode().ljust(16, b'\0'), 'invalid entry ID or padding')
        offset, packed_len, raw_len, packed_crc, raw_crc, flags = struct.unpack_from('<6I', entry, 16)
        require(offset == cursor and offset % 4096 == 0, 'payload range/ordering/alignment invalid')
        require(packed_len > 0 and packed_len <= total_used - offset, 'payload length out of bounds')
        require(flags == 1, 'unsupported compression flags')
        require(entry[104:] == b'\0' * 24, 'entry reserved bytes nonzero')
        packed = image[offset:offset + packed_len]
        require(crc(packed) == packed_crc, 'compressed CRC32 mismatch: ' + expected_id)
        require(hashlib.sha256(packed).digest() == entry[40:72], 'compressed SHA256 mismatch: ' + expected_id)
        raw = decompress_exact(packed, raw_len)
        require(crc(raw) == raw_crc, 'raw CRC32 mismatch: ' + expected_id)
        require(hashlib.sha256(raw).digest() == entry[72:104], 'raw SHA256 mismatch: ' + expected_id)
        row, spec = rows[i], specs[i]
        require(row.get('source_sha256') == spec.get('source_sha256'), 'source provenance mismatch')
        require(row.get('ttf_bytes') == raw_len and row.get('deflate_bytes') == packed_len, 'manifest payload length mismatch')
        require(hex_field(row.get('ttf_sha256'), 64, 'raw SHA256') == sha(raw) and
                hex_field(row.get('deflate_sha256'), 64, 'compressed SHA256') == sha(packed), 'manifest payload SHA256 mismatch')
        require(hex_field(row.get('ttf_crc32'), 8, 'raw CRC32') == f'{raw_crc:08x}' and
                hex_field(row.get('deflate_crc32'), 8, 'compressed CRC32') == f'{packed_crc:08x}', 'manifest payload CRC32 mismatch')
        next_offset = align(offset + packed_len)
        require(next_offset <= total_used, 'payload alignment exceeds total_used')
        require(image[offset + packed_len:next_offset] == b'\xff' * (next_offset - offset - packed_len),
                'payload alignment padding not erased')
        entries.append({'id': expected_id, 'offset': offset, 'offset_hex': f'0x{offset:x}',
                        'deflate_bytes': packed_len, 'raw_bytes': raw_len,
                        'deflate_crc32': f'{packed_crc:08x}', 'raw_crc32': f'{raw_crc:08x}',
                        'deflate_sha256': sha(packed), 'raw_sha256': sha(raw),
                        'end': offset + packed_len, 'aligned_end': next_offset})
        cursor = next_offset
    require(cursor == total_used, 'total_used does not equal aligned payload end')
    require(image[total_used:] == b'\xff' * (CAPACITY - total_used), 'tail guard not erased')
    return {'format': 'FOLFONT1', 'version': 1, 'bytes': len(image), 'sha256': sha(image),
            'crc32': f'{crc(image):08x}', 'header_crc32': f'{u32(header, 44):08x}',
            'metadata_offset': meta_offset, 'metadata_bytes': meta_len,
            'metadata_crc32': f'{u32(header, 40):08x}', 'metadata_sha256': sha(meta_raw),
            'total_used': total_used, 'erased_tail_bytes': CAPACITY - total_used,
            'minimum_guard_bytes': GUARD_BYTES, 'extra_after_guard_bytes': CAPACITY - total_used - GUARD_BYTES,
            'entries': entries, 'verified': True,
            'verification': ['bounds/order/alignment', 'all zero and FF padding', 'header/metadata CRC32',
                             'all payload CRC32 and SHA256', 'raw DEFLATE exact eof/length/no suffix',
                             'manifest/source/notice consistency', '64KiB erased tail guard'],
            'device_operation': False}


def asset_file(assets, relative):
    require(isinstance(relative, str) and not Path(relative).is_absolute() and '..' not in Path(relative).parts,
            'unsafe asset path')
    result = (assets / relative).resolve()
    require(assets in result.parents, 'asset path escapes directory')
    return result.read_bytes()


def build_image(assets):
    manifest = load_json(asset_file(assets, 'manifest.json'))
    source_raw = asset_file(assets, 'source-manifest.json')
    sources = load_json(source_raw)
    notice_raw = asset_file(assets, 'NOTICE.txt')
    require(sha(source_raw) == manifest.get('source_manifest_sha256'), 'source lock hash mismatch')
    metadata = json_bytes({'manifest': manifest, 'sources': sources, 'notice': notice_raw.decode('utf-8')}, compact=True)
    require(len(metadata) <= DATA_OFFSET - HEADER_BYTES, 'metadata exceeds 45056-byte reservation')
    image = bytearray(b'\xff' * CAPACITY)
    header = bytearray(HEADER_BYTES)
    header[:8] = MAGIC
    for offset, value in ((8, 1), (12, HEADER_BYTES), (20, 4), (24, ENTRY_BYTES),
                          (28, ENTRY_OFFSET), (32, HEADER_BYTES), (36, len(metadata)), (40, crc(metadata))):
        put32(header, offset, value)
    image[HEADER_BYTES:HEADER_BYTES + len(metadata)] = metadata
    rows = manifest.get('fonts', [])
    require(len(rows) == 4 and tuple(r.get('id') for r in rows) == IDS, 'builder requires exactly hei/kai/yuan/symbols')
    cursor = DATA_OFFSET
    for i, row in enumerate(rows):
        raw = asset_file(assets, row['ttf_path'])
        packed = asset_file(assets, row['deflate_path'])
        require(raw == decompress_exact(packed, row['ttf_bytes']), 'source TTF/deflate mismatch')
        require(sha(raw) == row['ttf_sha256'] and sha(packed) == row['deflate_sha256'], 'asset SHA256 mismatch')
        require(len(packed) == row['deflate_bytes'] and f'{crc(raw):08x}' == row['ttf_crc32'] and
                f'{crc(packed):08x}' == row['deflate_crc32'], 'asset CRC32/length mismatch')
        base = ENTRY_OFFSET + i * ENTRY_BYTES
        header[base:base + 16] = row['id'].encode().ljust(16, b'\0')
        struct.pack_into('<6I', header, base + 16, cursor, len(packed), len(raw), crc(packed), crc(raw), 1)
        header[base + 40:base + 72] = hashlib.sha256(packed).digest()
        header[base + 72:base + 104] = hashlib.sha256(raw).digest()
        require(cursor + len(packed) <= CAPACITY - GUARD_BYTES, 'font payload exceeds reserved guard')
        image[cursor:cursor + len(packed)] = packed
        cursor = align(cursor + len(packed))
    require(cursor <= CAPACITY - GUARD_BYTES, 'aligned image exceeds reserved guard')
    put32(header, 16, cursor)
    put32(header, 44, header_crc(header))
    image[:HEADER_BYTES] = header
    verify_image(image)
    return bytes(image)


def self_test(image):
    tests = []
    def reject(name, data):
        try:
            verify_image(data)
        except (PackError, ValueError, KeyError, TypeError) as exc:
            tests.append({'case': name, 'rejected': True, 'reason': str(exc)})
        else:
            raise PackError('negative test accepted: ' + name)
    reject('truncated_image', image[:-1])
    reject('oversize_image', image + b'\xff')
    for name, offset in (('magic_bit', 0), ('header_crc_bit', 44), ('metadata_bit', 4096),
                         ('payload_bit', DATA_OFFSET + 100), ('tail_non_ff', CAPACITY - 1)):
        bad = bytearray(image); bad[offset] ^= 1; reject(name, bad)
    changes = [('unsupported_version', 8, 2), ('wrong_count', 20, 3),
               ('metadata_outside_reservation', 36, DATA_OFFSET),
               ('total_used_consumes_guard', 16, CAPACITY),
               ('overlap_payload', ENTRY_OFFSET + ENTRY_BYTES + 16, DATA_OFFSET),
               ('payload_out_of_bounds', ENTRY_OFFSET + 20, CAPACITY),
               ('raw_over_6MiB', ENTRY_OFFSET + 24, MAX_RAW_BYTES + 1),
               ('unsupported_flags', ENTRY_OFFSET + 36, 2),
               ('raw_crc_mismatch', ENTRY_OFFSET + 32, 0)]
    for name, offset, value in changes:
        bad = bytearray(image); put32(bad, offset, value)
        put32(bad, 44, header_crc(bad[:HEADER_BYTES])); reject(name, bad)
    for name, offset in (('header_reserved_nonzero', 48), ('entry_reserved_nonzero', ENTRY_OFFSET + 104),
                         ('header_padding_nonzero', ENTRY_OFFSET + 4 * ENTRY_BYTES),
                         ('compressed_sha_mismatch', ENTRY_OFFSET + 40), ('raw_sha_mismatch', ENTRY_OFFSET + 72)):
        bad = bytearray(image); bad[offset] ^= 1
        put32(bad, 44, header_crc(bad[:HEADER_BYTES])); reject(name, bad)
    metadata_end = HEADER_BYTES + u32(image, 36)
    bad = bytearray(image); bad[metadata_end] = 0; reject('metadata_padding_non_ff', bad)
    payload_end = DATA_OFFSET + u32(image, ENTRY_OFFSET + 20)
    bad = bytearray(image); bad[payload_end] = 0; reject('payload_padding_non_ff', bad)
    base = ENTRY_OFFSET + 3 * ENTRY_BYTES
    offset, count, raw_len = [u32(image, base + n) for n in (16, 20, 24)]
    packed = image[offset:offset + count]
    for name, data in (('deflate_truncated', packed[:-1]), ('deflate_trailing_suffix', packed + b'\0')):
        try:
            decompress_exact(data, raw_len)
        except PackError as exc:
            tests.append({'case': name, 'rejected': True, 'reason': str(exc)})
        else:
            raise PackError('negative deflate accepted: ' + name)
    return tests


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets', type=Path, default=ROOT / 'main/assets/fonts')
    p.add_argument('--output', type=Path, default=ROOT / 'main/assets/folio_fonts.bin')
    p.add_argument('--report', type=Path, default=ROOT / 'artifacts/firmware-0830/font-assets/pack-report.json')
    p.add_argument('--verify-only', action='store_true')
    p.add_argument('--self-test', action='store_true')
    args = p.parse_args()
    image = args.output.read_bytes() if args.verify_only else build_image(args.assets.resolve())
    result = verify_image(image)
    if args.self_test:
        result['negative_tests'] = self_test(image)
    if not args.verify_only:
        # Restrict binary output to the named new asset or this task's evidence tree.
        # In particular, a symlink/device path cannot turn a build into a flash write.
        destination = args.output.resolve()
        evidence_root = ROOT / 'artifacts/firmware-0830/font-assets'
        require(args.output.suffix == '.bin' and not args.output.is_symlink() and
                (destination == ROOT / 'main/assets/folio_fonts.bin' or evidence_root in destination.parents),
                'output must be the new local asset or task evidence .bin')
        require(not args.output.exists() or args.output.is_file(), 'output is not a regular file')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(image)
        require(args.output.read_bytes() == image, 'written image differs')
    result['image_path'] = str(args.output.resolve().relative_to(ROOT)) if args.output.resolve().is_relative_to(ROOT) else str(args.output.resolve())
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_bytes(json_bytes(result))
    print(json.dumps({k: v for k, v in result.items() if k != 'negative_tests'}, ensure_ascii=False, indent=2))
    if args.self_test:
        print('Negative cases rejected:', len(result['negative_tests']))


if __name__ == '__main__':
    main()
