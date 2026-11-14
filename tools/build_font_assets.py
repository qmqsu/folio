#!/usr/bin/env python3
"""Build/verify pinned, renamed OFL font assets without installing or flashing.

Run with the isolated fontTools 4.68.0 environment recorded in the source lock.
Defaults write only NEW main/assets/fonts and artifacts/firmware-0830/font-assets.
No network, archive execution, source-font rewrite, resource-container or USB I/O.
The original Songti is read-only. --verify-only never rewrites production assets.
"""
import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import zlib

import fontTools
from fontTools import subset
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
PRIMARY_IDS = {1, 2, 3, 4, 6, 16, 17, 18, 21, 22, 25}
SAMPLES = {0x56FD, 0x9605, 0x6C38, 0x95F4, 0x4E66}
PUNCTUATION = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0xB7}
MAX_BYTES = 6 * 1024 * 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def crc(data):
    return f'{zlib.crc32(data) & 0xffffffff:08x}'


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode()


def cp_list(chars):
    return [f'U+{cp:04X}' for cp in sorted(chars)]


def charset_bytes(chars):
    return ('\n'.join(cp_list(chars)) + '\n').encode()


def write(base, relative, data):
    path = (base / relative).resolve()
    if base not in path.parents:
        raise ValueError('output escaped its designated directory')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def checked_source(path, digest, size=None):
    data = (ROOT / path).read_bytes()
    if sha(data) != digest or (size is not None and len(data) != size):
        raise ValueError(f'pinned input mismatch: {path}')
    return data


def cmap4(raw):
    """Independent decode of firmware-selected LAST Unicode format-4 cmap."""
    tables = {}
    for i in range(struct.unpack_from('>H', raw, 4)[0]):
        tag, _, offset, size = struct.unpack_from('>4sIII', raw, 12 + 16 * i)
        tables[tag] = raw[offset:offset + size]
    table = tables[b'cmap']
    mapping, segments = None, None
    for i in range(struct.unpack_from('>H', table, 2)[0]):
        platform, encoding, offset = struct.unpack_from('>HHI', table, 4 + 8 * i)
        if not (platform == 0 or platform == 3 and encoding in (1, 10)):
            continue
        if struct.unpack_from('>H', table, offset)[0] != 4:
            continue
        sub = table[offset:]
        n = struct.unpack_from('>H', sub, 6)[0] // 2
        mapping, segments = {}, n
        for j in range(n):
            end = struct.unpack_from('>H', sub, 14 + 2 * j)[0]
            start, delta, distance = [struct.unpack_from('>H', sub, base + 2 * j)[0]
                                      for base in (16 + 2 * n, 16 + 4 * n, 16 + 6 * n)]
            for cp in range(start, end + 1):
                gid = struct.unpack_from('>H', sub, 16 + 6 * n + 2 * j + distance + 2 * (cp - start))[0] if distance else cp
                if not distance or gid:
                    gid = (gid + delta) & 0xffff
                if gid and cp != 0xffff:
                    mapping[cp] = gid
    if mapping is None or segments > 4096:
        raise ValueError('no renderer-compatible cmap4')
    return mapping, segments


def scan_charsets(songti_raw):
    a = set(cmap4(songti_raw)[0])
    assert len(a) == 4538
    gb = set()
    for first in range(0xB0, 0xF8):
        for second in range(0xA1, 0xFF):
            try:
                gb.add(ord(bytes([first, second]).decode('gb2312', errors='strict')))
            except UnicodeDecodeError:
                pass
    assert len(gb) == 6763 and all(0x4E00 <= cp <= 0x9FFF for cp in gb)
    ui, inputs = set(PUNCTUATION), []
    for path in sorted((ROOT / 'main').rglob('*')):
        if path.suffix in ('.c', '.h', '.inc', '.cpp') and 'vendor' not in path.parts:
            raw = path.read_bytes()
            ui.update(ord(c) for c in raw.decode() if 0x3000 <= ord(c) <= 0xFFEF)
            inputs.append({'path': str(path.relative_to(ROOT)), 'sha256': sha(raw)})
    b = a | gb | ui | set(range(32, 127))
    assert all(cp < 0xFFFF and not 0xD800 <= cp <= 0xDFFF for cp in b)
    return {'A': a, 'B': b, 'GB2312_HAN': gb, 'CURRENT_UI_SOURCE_SCAN': ui}, inputs


def notices(font):
    return sorted((n.platformID, n.platEncID, n.langID, n.nameID, n.string)
                  for n in font['name'].names if n.nameID not in PRIMARY_IDS)


def rename(font, spec, target_hash):
    family, style = spec['family'], spec['style']
    compact = family.replace(' ', '')
    names = {1: family, 2: style, 3: f'{compact}-{style}-0830-{target_hash[:12]}',
             4: f'{family} {style}', 6: f'{compact}-{style}', 16: family,
             17: style, 18: f'{family} {style}', 21: family, 22: style, 25: compact}
    for record in font['name'].names:
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding())
    # Always expose canonical Unicode Windows primary naming, even if absent upstream.
    for name_id in (1, 2, 3, 4, 6, 16, 17):
        font['name'].setName(names[name_id], name_id, 3, 1, 0x409)


def build_font(raw, spec, wanted):
    font = TTFont(io.BytesIO(raw), recalcTimestamp=False)
    original_names = copy.deepcopy(font['name'])
    options = subset.Options()
    options.hinting = False
    options.layout_features = []
    options.bidi_closure = False
    options.name_IDs = ['*']
    options.name_languages = ['*']
    options.name_legacy = True
    worker = subset.Subsetter(options=options)
    worker.populate(unicodes=wanted)
    worker.subset(font)
    for table in ('GPOS', 'GSUB', 'GDEF', 'BASE', 'DSIG'):
        if table in font:
            del font[table]
    font['name'] = original_names
    rename(font, spec, sha(charset_bytes(wanted)))
    output = io.BytesIO()
    font.save(output, reorderTables=True)
    raw = output.getvalue()
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = compressor.compress(raw) + compressor.flush()
    return raw, packed


def inspect_font(raw, packed, upstream, spec, wanted):
    assert len(raw) <= MAX_BYTES and zlib.decompress(packed, -15) == raw
    assert sum(struct.unpack('>' + 'I' * ((len(raw) + 3) // 4), raw + b'\0' * (-len(raw) % 4))) & 0xffffffff == 0xB1B0AFBA
    font = TTFont(io.BytesIO(raw), recalcTimestamp=False)
    source = TTFont(io.BytesIO(upstream), recalcTimestamp=False)
    assert 'glyf' in font and 'loca' in font and 'fvar' not in font
    assert font['OS/2'].usWeightClass == spec['weight']
    assert notices(source) == notices(font), 'non-primary legal/name metadata changed'
    for record in font['name'].names:
        if record.nameID in PRIMARY_IDS:
            assert not any(rfn.casefold() in record.toUnicode().casefold() for rfn in spec['reserved_font_names'])
    source_map, new_map = source.getBestCmap(), font.getBestCmap()
    expected = wanted & set(source_map)
    firmware_map, segments = cmap4(raw)
    assert expected == set(new_map) == set(firmware_map)
    old_glyphs, new_glyphs = source.getGlyphSet(), font.getGlyphSet()
    digest, max_points = hashlib.sha256(), 0
    for cp in sorted(expected):
        old_name, new_name = source_map[cp], new_map[cp]
        p1, p2 = DecomposingRecordingPen(old_glyphs), DecomposingRecordingPen(new_glyphs)
        old_glyphs[old_name].draw(p1)
        new_glyphs[new_name].draw(p2)
        assert p1.value == p2.value, f'outline changed at U+{cp:04X}'
        assert source['hmtx'][old_name] == font['hmtx'][new_name], f'hmtx changed at U+{cp:04X}'
        digest.update(repr((cp, p2.value, font['hmtx'][new_name])).encode())
    for glyph in font['glyf'].glyphs.values():
        glyph.expand(font['glyf'])
        if glyph.numberOfContours > 0:
            max_points = max(max_points, len(glyph.coordinates))
    nglyph = font['maxp'].numGlyphs
    # The simple-glyph flag allocation is freed before the four tree arrays.
    validator_arrays = nglyph * (1 + 4 + 1 + 2)
    metrics = {}
    for cp in sorted(SAMPLES & set(new_map)):
        g = font['glyf'][new_map[cp]]
        metrics[f'U+{cp:04X}'] = [g.xMin, g.yMin, g.xMax, g.yMax]
    return {
        'ttf_bytes': len(raw), 'deflate_bytes': len(packed), 'ttf_sha256': sha(raw),
        'deflate_sha256': sha(packed), 'ttf_crc32': crc(raw), 'deflate_crc32': crc(packed),
        'codepoint_count': len(expected), 'target_count': len(wanted),
        'missing_codepoints': cp_list(wanted - expected), 'glyph_count': nglyph,
        'cmap4_segments': segments, 'static_glyf_cmap4': True, 'weight': spec['weight'],
        'outline_and_advance_checked_count': len(expected), 'outline_or_hmtx_mismatches': 0,
        'outline_metrics_digest_sha256': digest.hexdigest(),
        'unmodified_non_primary_name_metadata': True,
        'all_primary_names_renamed': True,
        'normalization_sample_bboxes': metrics,
        'allocation_budget': {
            'raw_ttf_bytes': len(raw), 'validator_tree_arrays_bytes': validator_arrays,
            'validator_simple_flags_max_bytes': max_points,
            'raw_plus_validator_peak_bytes': len(raw) + max(validator_arrays, max_points),
            'excludes': 'font state, inflater, input buffers, allocator overhead, glyph raster/cache, existing fonts, framebuffers, reader/network allocations; not device PSRAM measurement'
        }
    }


def native_validator(evidence, paths):
    source = (ROOT / 'main/book/folio_font.c').read_text()
    helpers = source[source.index('typedef struct {\n    const uint8_t* data;'):source.index('// The renderer only consumes glyph indices.')]
    body = source[source.index('static bool read_loca('):source.index('static void* font_alloc(')]
    prefix = '''#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n#define FOLIO_FONT_MAX_BYTES (6u * 1024u * 1024u)\n#define FOLIO_FONT_MAX_EXPANDED_POINTS 8192\n#define FOLIO_FONT_MAX_COMPONENT_DEPTH 16\nstatic void font_yield(void) {}\n'''
    main = r'''
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        FILE* file = fopen(argv[i], "rb"); if (!file) return 2;
        if (fseek(file, 0, SEEK_END)) return 3;
        long n = ftell(file); if (n < 0 || fseek(file, 0, SEEK_SET)) return 4;
        uint8_t* raw = malloc((size_t)n); if (!raw) return 5;
        if (fread(raw, 1, (size_t)n, file) != (size_t)n) return 6;
        fclose(file); uint32_t offset = 0;
        bool okay = validate_ttf(raw, (size_t)n, &offset);
        printf("{\"file\":\"%s\",\"valid\":%s,\"cmap4_offset\":%u}\n", argv[i], okay ? "true" : "false", offset);
        free(raw); if (!okay) return 7;
    }
    return 0;
}
'''
    cpath = write(evidence, 'validator-host.c', (prefix + helpers + body + main).encode())
    binary = evidence / 'validator-host'
    command = ['/usr/bin/clang', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', str(cpath), '-o', str(binary)]
    result = subprocess.run(command, capture_output=True)
    write(evidence, 'validator-build.log', result.stdout + result.stderr)
    result.check_returncode()
    result = subprocess.run([str(binary)] + [str(p) for p in paths], capture_output=True)
    write(evidence, 'validator-results.jsonl', result.stdout)
    write(evidence, 'validator-run.log', result.stderr)
    result.check_returncode()
    return {'renderer_source_sha256': sha(source.encode()), 'host_sanitizers': ['ASan', 'UBSan'],
            'source_extract_sha256': sha((helpers + body).encode()),
            'all_valid': all(json.loads(line)['valid'] for line in result.stdout.splitlines())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'main/assets/fonts')
    parser.add_argument('--evidence', type=Path, default=ROOT / 'artifacts/firmware-0830/font-assets')
    parser.add_argument('--sources', type=Path, default=ROOT / 'tools/font_sources_0830.json')
    parser.add_argument('--charset', type=Path, help='Pinned B.txt for exact reproduction; must cover current A/GB2312/UI')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    out, evidence = args.output.resolve(), args.evidence.resolve()
    if out == ROOT / 'main/assets' or out in (ROOT, ROOT / 'main'):
        raise ValueError('refusing output path containing the original Songti')
    lock_raw = args.sources.read_bytes()
    lock = json.loads(lock_raw)
    assert fontTools.version == lock['fonttools_version']
    assert zlib.ZLIB_RUNTIME_VERSION == lock['zlib_runtime_version'], 'compression version mismatch; do not silently change locked bytes'
    original = lock['existing_songti']
    songti_raw = checked_source(original['path'], original['sha256'])
    songti_packed = checked_source(original['deflate_path'], original['deflate_sha256'])
    assert zlib.decompress(songti_packed, -15) == songti_raw
    sets, ui_inputs = scan_charsets(songti_raw)
    if args.charset:
        pinned = {int(cp[2:], 16) for cp in args.charset.read_text().splitlines()}
        assert sets['B'] <= pinned, 'pinned B omits current required characters'
        sets['B'] = pinned
    if args.verify_only:
        manifest = json.loads((out / 'manifest.json').read_text())
        assert (out / 'source-manifest.json').read_bytes() == lock_raw
        assert manifest['source_manifest_sha256'] == sha(lock_raw)
        for name, record in manifest['charsets'].items():
            data = (out / record['path']).read_bytes()
            assert sha(data) == record['sha256'], f'charset hash mismatch: {name}'
            assert len(data.decode().splitlines()) == record['count']
        pinned = {int(cp[2:], 16) for cp in (out / 'charsets/B.txt').read_text().splitlines()}
        assert sets['B'] <= pinned, 'generated assets no longer cover current UI'
        sets['B'] = pinned
    target = sets['B']
    yuan_spec = next(s for s in lock['fonts'] if s['id'] == 'yuan')
    yuan_raw = checked_source(yuan_spec['input_path'], yuan_spec['source_sha256'], yuan_spec['source_bytes'])
    missing = target - set(TTFont(io.BytesIO(yuan_raw)).getBestCmap())
    assert len(missing) == 38 and missing <= sets['A'], 'fallback contract changed; stop for review'
    symbols = missing | SAMPLES
    sets['SYMBOLS_REQUIRED'], sets['SYMBOLS_CALIBRATION'], sets['SYMBOLS_ALL'] = missing, SAMPLES, symbols
    rows, coverage, paths = [], {}, []
    notice_parts = [
        'Folio embedded OFL font derivatives (asset revision 0830-1)\n',
        'Modified versions; not endorsed by upstream authors. All derived fonts remain under SIL OFL 1.1.\n',
        'Modifications: exact B subsetting; remove hints and OpenType layout; rename all primary names in every retained language; preserve original retained outlines and advances.\n',
        'B = unchanged Songti A (4538) + all 6763 GB2312 Han + printable ASCII + conservative current native UI source scan. Not full Unicode.\n',
        'Folio Yuan requires separate Folio Symbols for its 38 missing codepoints. Symbols also retains 5 unchanged CJK calibration glyphs, and is not a user-selectable font. Original Songti is unchanged.\n',
        'Do not use reserved upstream family names as the primary displayed name of these modified fonts. Upstream names below identify source attribution only.\n',
        'Distribute this readable notice and all accompanying license files with every firmware/resource/source bundle; do not rely solely on inaccessible flash metadata.\n'
    ]
    for spec in lock['fonts']:
        upstream = checked_source(spec['input_path'], spec['source_sha256'], spec['source_bytes'])
        license_raw = checked_source(spec['license_input'], spec['license_sha256'])
        wanted = symbols if spec['id'] == 'symbols' else target
        stem = 'folio_' + spec['id']
        ttf_path, deflate_path = stem + '.ttf', stem + '.deflate'
        if args.verify_only:
            raw, packed = (out / ttf_path).read_bytes(), (out / deflate_path).read_bytes()
            old = next(row for row in manifest['fonts'] if row['id'] == spec['id'])
            assert sha(raw) == old['ttf_sha256'] and sha(packed) == old['deflate_sha256']
            assert (out / spec['license_path']).read_bytes() == license_raw
        else:
            raw, packed = build_font(upstream, spec, wanted)
            write(out, ttf_path, raw)
            write(out, deflate_path, packed)
            write(out, spec['license_path'], license_raw)
        row = {key: spec[key] for key in ('id', 'family', 'style', 'upstream', 'version', 'source_sha256', 'license_path')}
        row.update({'ttf_path': ttf_path, 'deflate_path': deflate_path,
                    'fallback_id': 'symbols' if spec['id'] == 'yuan' else None,
                    'selectable': spec['id'] != 'symbols',
                    'calibration_cps': cp_list(SAMPLES) if spec['id'] == 'symbols' else [],
                    'required_fallback_cps': cp_list(missing) if spec['id'] in ('yuan', 'symbols') else []})
        row.update(inspect_font(raw, packed, upstream, spec, wanted))
        rows.append(row)
        coverage[spec['id']] = set(cmap4(raw)[0])
        paths.append(out / ttf_path)
        notice_parts.append(f"\n{spec['family']} {spec['style']} is derived from {spec['upstream']} {spec['version']}.\nSource: {spec['repo']}\nSource TTF SHA-256: {spec['source_sha256']}\nFull original copyright and license ({spec['license_path']}):\n\n" + license_raw.decode() + '\n')
        print(json.dumps({'id': spec['id'], 'raw': len(raw), 'deflate': len(packed), 'cmap': len(coverage[spec['id']]), 'missing': len(wanted - coverage[spec['id']])}), flush=True)
    assert coverage['hei'] == target == coverage['kai']
    assert target <= coverage['yuan'] | coverage['symbols']
    assert sets['A'] <= target and sets['GB2312_HAN'] <= target and sets['CURRENT_UI_SOURCE_SCAN'] <= target
    assert coverage['symbols'] == symbols
    charsets = {name: {'path': f'charsets/{name}.txt', 'count': len(chars), 'sha256': sha(charset_bytes(chars))}
                for name, chars in sets.items()}
    notice = '\n'.join(notice_parts).encode()
    result = {'schema': 'folio-font-assets-v1', 'revision': '0830-1',
              'source_manifest': 'source-manifest.json', 'source_manifest_sha256': sha(lock_raw),
              'builder': 'tools/build_font_assets.py',
              'compression': {'format': 'raw-deflate', 'level': 9, 'wbits': -15, 'zlib': zlib.ZLIB_RUNTIME_VERSION},
              'checksum': 'CRC32 IEEE as zlib.crc32(data, 0) / mz_crc32(0, data, len); stored as 8 lowercase hex digits; SHA256 authenticates pinned sources and artifacts, not a signature',
              'charsets': charsets, 'fonts': rows,
              'existing_songti': original,
              'coverage': {'hei_B_complete': True, 'kai_B_complete': True, 'yuan_plus_symbols_B_complete': True,
                           'symbols_required_count': len(missing), 'symbols_calibration_count': len(SAMPLES),
                           'A_preserved_count': len(sets['A']), 'B_count': len(target),
                           'songti_remains_A_only': True},
              'notice': {'path': 'NOTICE.txt', 'bytes': len(notice), 'sha256': sha(notice)},
              'resource_container': 'Not generated by this tool; pack only deflates plus bounded manifest/notices under an independently agreed format.'}
    if args.verify_only:
        # Current source scan may have grown within already-covered B; asset identity is unchanged.
        for row in rows:
            old = next(r for r in manifest['fonts'] if r['id'] == row['id'])
            assert row == old, f'manifest inspection mismatch: {row["id"]}'
        assert (out / 'NOTICE.txt').read_bytes() == notice
    else:
        for name, chars in sets.items():
            write(out, charsets[name]['path'], charset_bytes(chars))
        write(out, 'NOTICE.txt', notice)
        write(out, 'source-manifest.json', lock_raw)
        write(out, 'manifest.json', json_bytes(result))
    write(evidence, 'ui-inputs.json', json_bytes(ui_inputs))
    write(evidence, 'coverage-current.json', json_bytes({'charsets': charsets, 'ui_files': len(ui_inputs),
        'UI_scan_boundary': 'source scan includes comments; same suffix/vendor/range/punctuation policy as original builder, not semantic extraction',
        'missing_A_hei': cp_list(sets['A'] - coverage['hei']), 'missing_A_kai': cp_list(sets['A'] - coverage['kai']),
        'missing_A_yuan_combined': cp_list(sets['A'] - coverage['yuan'] - coverage['symbols']),
        'missing_B_yuan_combined': cp_list(target - coverage['yuan'] - coverage['symbols']),
        'required_fallback_cps': cp_list(missing), 'calibration_cps': cp_list(SAMPLES)}))
    validation = native_validator(evidence, paths)
    write(evidence, 'validation.json', json_bytes(validation))
    write(evidence, 'font-measurements.json', json_bytes({'fonts': rows,
        'total_deflate_bytes': sum(r['deflate_bytes'] for r in rows),
        'largest_single_raw_ttf_bytes': max(r['ttf_bytes'] for r in rows),
        'largest_raw_plus_validator_peak_bytes': max(r['allocation_budget']['raw_plus_validator_peak_bytes'] for r in rows),
        'native_validator': validation, 'device_psram_peak_measured': False}))
    assert checked_source(original['path'], original['sha256']) == songti_raw
    assert checked_source(original['deflate_path'], original['deflate_sha256']) == songti_packed
    print(json.dumps({'verified': True, 'B': len(target), 'total_deflate_bytes': sum(r['deflate_bytes'] for r in rows),
                      'native_validator': validation, 'original_songti_unchanged': True}, indent=2))


if __name__ == '__main__':
    main()
