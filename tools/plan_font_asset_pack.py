#!/usr/bin/env python3
"""Measure a proposed read-only resource layout. Never builds/writes a flash image.

4096-byte header and four sector-aligned raw DEFLATE entries. Metadata may be
packed densely in a 48KiB aggregate or separately sector-aligned. In either case
reserve a further 64KiB untouched guard inside the existing 5MiB capacity.
An arithmetically fitting proposal does not authorize using occupied storage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parents[1]
ALIGNMENT = 4096
CAPACITY = 5 * 1024 * 1024
GUARD = 64 * 1024


def align(value):
    return (value + ALIGNMENT - 1) // ALIGNMENT * ALIGNMENT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets', type=Path, default=ROOT / 'main/assets/fonts')
    p.add_argument('--output', type=Path, default=ROOT / 'artifacts/firmware-0830/font-assets/packing-proposal.json')
    args = p.parse_args()
    manifest = json.loads((args.assets / 'manifest.json').read_text())
    for row in manifest['fonts']:
        for kind in ('ttf', 'deflate'):
            data = (args.assets / row[kind + '_path']).read_bytes()
            assert len(data) == row[kind + '_bytes']
            assert hashlib.sha256(data).hexdigest() == row[kind + '_sha256']
            assert f'{zlib.crc32(data) & 0xffffffff:08x}' == row[kind + '_crc32']
    meta = [{'path': name, 'bytes': (args.assets / name).stat().st_size}
            for name in ('manifest.json', 'source-manifest.json', 'NOTICE.txt')]
    proposals = []
    for dense in (True, False):
        offset = ALIGNMENT
        entries = []
        for row in meta:
            entries.append(row | {'offset': offset})
            offset += row['bytes'] if dense else align(row['bytes'])
        actual_metadata_end = offset
        offset = 48 * 1024 if dense else offset
        assert offset >= actual_metadata_end
        fonts_start = offset
        for row in manifest['fonts']:
            entries.append({'id': row['id'], 'path': row['deflate_path'], 'offset': offset,
                            'bytes': row['deflate_bytes'], 'allocated_bytes': align(row['deflate_bytes'])})
            offset += align(row['deflate_bytes'])
        assert offset < CAPACITY and offset + GUARD <= CAPACITY
        proposals.append({'layout': 'dense-metadata-in-48KiB-region' if dense else 'each-metadata-file-sector-aligned',
            'header_reservation_bytes': ALIGNMENT, 'metadata_actual_end': actual_metadata_end,
            'first_font_offset': fonts_start, 'entries': entries,
            'container_sector_padded_bytes': offset, 'capacity_bytes': CAPACITY,
            'remaining_bytes': CAPACITY - offset, 'guard_bytes': GUARD,
            'remaining_after_guard_bytes': CAPACITY - offset - GUARD,
            'arithmetic_fits_with_guard': True})
    result = {'schema': 'folio-font-packing-proposal-v1', 'status': 'PROPOSAL ONLY; format and occupancy not authorized/settled',
              'partition_address_context': '0x410000 (existing storage); no partition or device operation performed',
              'alignment': ALIGNMENT, 'font_payload_bytes': sum(r['deflate_bytes'] for r in manifest['fonts']),
              'notice_contains_full_licenses': True,
              'excluded_from_resource': ['raw .ttf', 'upstream originals', 'duplicate individual license files', 'charset text files', 'host evidence'],
              'keep_in_distribution': ['manifest.json', 'source-manifest.json', 'NOTICE.txt', 'licenses/*', 'generator and pinned source lock'],
              'proposals': proposals}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
