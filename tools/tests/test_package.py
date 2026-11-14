#!/usr/bin/env python3
"""Packaging regression: high OTA data must never pad across existing FAT."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('folio_package', ROOT / 'tools/package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def partition_bytes(partitions=None):
    entries = b''.join(struct.pack('<HBBII16sI', 0x50AA, kind, subtype, offset, size, name.encode(), 0)
                       for name, (kind, subtype, offset, size) in (partitions or package.EXPECTED_PARTITIONS).items())
    return (entries + b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(entries).digest()).ljust(0xC00, b'\xff')


def main():
    with tempfile.TemporaryDirectory(prefix='folio-package-') as temporary:
        temp = Path(temporary); root = temp / 'checkout'; root.mkdir(); build = temp / 'build'; build.mkdir()
        for name in ('main', 'components', 'tools', 'docs', 'preview', 'licenses'):
            (root / name).mkdir()
        for name in ('CMakeLists.txt', 'sdkconfig.defaults', 'sdkconfig', 'partitions_16M.csv', 'dependencies.lock',
                     'LICENSES.md', 'README.md', '.gitignore', 'docs/刷机说明.md', 'main/app.c', 'licenses/NOTICE'):
            (root / name).write_text('fixture\n'); os.utime(root / name, (1, 1))
        flash = {'flash_files': {'0x0': 'boot.bin', '0x8000': 'partitions.bin', '0x10000': 'app.bin', '0x910000': 'ota.bin'},
                 'bootloader': {'offset': '0x0', 'file': 'boot.bin', 'encrypted': 'false'},
                 'partition-table': {'offset': '0x8000', 'file': 'partitions.bin', 'encrypted': 'false'},
                 'app': {'offset': '0x10000', 'file': 'app.bin', 'encrypted': 'false'},
                 'extra_esptool_args': {'chip': 'esp32s3'},
                 'flash_settings': {'flash_size': '16MB', 'flash_mode': 'dio', 'flash_freq': '80m'}}
        description = {'project_name': 'Folio', 'target': 'esp32s3', 'project_path': str(root), 'project_version': 'test'}

        def reset():
            (build / 'flasher_args.json').write_text(json.dumps(flash))
            (build / 'project_description.json').write_text(json.dumps(description))
            (build / 'boot.bin').write_bytes(b'B' * 0x4000)
            (build / 'partitions.bin').write_bytes(partition_bytes())
            (build / 'app.bin').write_bytes(b'A' * 0x10001)
            (build / 'ota.bin').write_bytes(b'\xff' * 0x2000)

        merges = []

        def fake_merge(command, check):
            assert check and 'merge-bin' in command and 'write-flash' not in command
            images = command[-6:]
            assert images[::2] == ['0x0', '0x8000', '0x10000']
            assert not any('ota_data' in arg or 'font-resources' in arg or arg in ('0x410000', '0x910000') for arg in command)
            data = bytearray()
            for offset, filename in zip(images[::2], images[1::2]):
                start = int(offset, 0); payload = Path(filename).read_bytes()
                data.extend(b'\xff' * max(0, start + len(payload) - len(data)))
                data[start:start + len(payload)] = payload
            Path(command[command.index('-o') + 1]).write_bytes(data)
            merges.append(command)

        reset()
        with patch.object(package.subprocess, 'run', side_effect=fake_merge):
            release = temp / 'release'; package.package_build(root, build, release)
        manifest = json.loads((release / 'manifest.json').read_text())
        assert manifest['layout_version'] == package.LAYOUT_VERSION
        assert manifest['flash_files'] == {'0x0': 'bootloader.bin', '0x8000': 'partition-table.bin', '0x10000': 'app.bin', '0x910000': 'ota_data_initial.bin'}
        assert manifest['usb_install_files'] == manifest['flash_files']
        assert manifest['optional_merged_install_files'] == {'0x0': 'merged.bin', '0x910000': 'ota_data_initial.bin'}
        assert manifest['optional_merged_install_overwrites_nvs']
        assert manifest['requires_separate_otadata_write']
        assert (release / 'merged.bin').stat().st_size == 0x20001 < 0x410000
        assert (release / 'ota_data_initial.bin').read_bytes() == b'\xff' * 0x2000
        assert (release / 'update/folio.bin').read_bytes() == (build / 'app.bin').read_bytes()
        assert (release / 'update/folio.bin.sha256').read_text() == package.digest(build / 'app.bin') + '  folio.bin\n'
        for name, expected in manifest['sha256'].items(): assert package.digest(release / name) == expected
        for line in (release / 'sha256.txt').read_text().splitlines():
            expected, name = line.split('  ', 1); assert package.digest(release / name) == expected
        command = (release / 'usb-install.txt').read_text()
        commands = [line for line in command.splitlines() if line.startswith('python -m esptool')]
        assert len(commands) == 2
        assert '0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 app.bin 0x910000 ota_data_initial.bin' in commands[0]
        assert '0x0 merged.bin 0x910000 ota_data_initial.bin' in commands[1]
        assert 'this overwrites NVS, including stored device settings and Wi-Fi credentials' in command
        count = 1

        def rejected(name, mutate, expected):
            nonlocal count
            reset(); mutate(); destination = temp / name
            with patch.object(package.subprocess, 'run', side_effect=fake_merge):
                try: package.package_build(root, build, destination)
                except (ValueError, OSError) as error: assert expected in str(error), (name, error)
                else: raise AssertionError(f'{name} unexpectedly packaged')
            assert not destination.exists(), 'Failed package should not leave a release directory'
            count += 1

        def flash_change(function):
            changed = copy.deepcopy(flash); function(changed)
            (build / 'flasher_args.json').write_text(json.dumps(changed))

        rejected('old-layout', lambda: flash_change(lambda value: value['flash_files'].pop('0x910000')), 'Unexpected flash offsets')
        rejected('extra-storage-write', lambda: flash_change(lambda value: value['flash_files'].update({'0x410000': 'app.bin'})), 'Unexpected flash offsets')
        rejected('oversized-app', lambda: (build / 'app.bin').write_bytes(b'A' * (0x300000 + 1)), '3 MiB')
        rejected('empty-app', lambda: (build / 'app.bin').write_bytes(b''), '3 MiB')
        rejected('oversized-boot', lambda: (build / 'boot.bin').write_bytes(b'B' * 0x8001), 'overlaps')
        rejected('uninitialized-ota', lambda: (build / 'ota.bin').write_bytes(b'\0' * 0x2000), '8192 erased')
        rejected('short-ota', lambda: (build / 'ota.bin').write_bytes(b'\xff' * 0x1000), '8192 erased')
        rejected('stale-app', lambda: os.utime(build / 'app.bin', (0, 0)), 'older than source')
        rejected('declared-offset', lambda: flash_change(lambda value: value['app'].update({'offset': '0x920000'})), 'Inconsistent app')
        rejected('wrong-chip', lambda: flash_change(lambda value: value['extra_esptool_args'].update({'chip': 'esp32'})), 'ESP32-S3')
        outside = temp / 'secret.bin'; outside.write_bytes(b'private')
        rejected('path-traversal', lambda: flash_change(lambda value: value['flash_files'].update({'0x0': '../secret.bin'})), 'escapes')
        rejected('absolute-file', lambda: flash_change(lambda value: value['flash_files'].update({'0x0': str(outside)})), 'relative')

        for name, replacement in [('moved-factory', ('factory', (0, 0, 0x20000, 0x400000))),
                                  ('moved-storage', ('storage', (1, 0x81, 0x420000, 0x500000))),
                                  ('overlap', ('ota_0', (0, 0x10, 0x900000, 0x300000))),
                                  ('past-flash', ('ota_1', (0, 0x11, 0xE00000, 0x300000)))]:
            changed = dict(package.EXPECTED_PARTITIONS); changed[replacement[0]] = replacement[1]
            rejected(name, lambda p=changed: (build / 'partitions.bin').write_bytes(partition_bytes(p)), '16 MB' if name == 'past-flash' else 'layout')
        bad_md5 = bytearray(partition_bytes()); bad_md5[32 * 7 + 16] ^= 1
        rejected('bad-md5', lambda: (build / 'partitions.bin').write_bytes(bad_md5), 'MD5')
        # Defense in depth if merge-bin ever pads an image into the FAT region.
        reset(); target = temp / 'bad-merge'
        def bad_merge(command, check):
            fake_merge(command, check)
            with Path(command[command.index('-o') + 1]).open('r+b') as stream: stream.truncate(0x910001)
        with patch.object(package.subprocess, 'run', side_effect=bad_merge):
            try: package.package_build(root, build, target)
            except ValueError as error: assert 'storage boundary' in str(error)
            else: raise AssertionError('High-offset merged image accepted')
        assert not target.exists(); count += 1
        reset()
        try: package.package_build(root, build, release)
        except FileExistsError: pass
        else: raise AssertionError('Existing release overwritten')
        assert (release / 'manifest.json').exists(); count += 1
        # The real shipped resource is copied into isolated fake checkouts. No
        # research/artifacts/fonts environment, network, serial or real esptool.
        validator = package.font_pack_module()
        real_image = (ROOT / 'main/assets/folio_fonts.bin').read_bytes()
        real_info = validator.verify_image(real_image)
        assets = ROOT / 'main/assets/fonts'
        source_record = json.loads((assets / 'source-manifest.json').read_text())
        readable_names = ['manifest.json', 'source-manifest.json', 'NOTICE.txt'] + [r['license_path'] for r in source_record['fonts']]
        readable = {name: (assets / name).read_bytes() for name in readable_names}

        def font_case(name):
            reset()
            base = temp / ('font-' + name); base.mkdir()
            case_root, case_build = base / 'checkout', base / 'build'
            shutil.copytree(root, case_root); shutil.copytree(build, case_build)
            (case_root / 'main/book').mkdir()
            loader = case_root / 'main/book/folio_font_pack.c'
            loader.write_text('/* loader presence fixture */\n'); os.utime(loader, (1, 1))
            for relative, raw in readable.items():
                path = case_root / 'main/assets/fonts' / relative
                path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(raw)
            (case_root / 'main/assets/folio_fonts.bin').write_bytes(real_image)
            old = case_root / 'main/assets/folio_builtin.deflate'; old.write_bytes(b'legacy-songti'); os.utime(old, (1, 1))
            for name in ('package.py', 'build_font_pack.py'):
                shutil.copy2(ROOT / 'tools' / name, case_root / 'tools' / name)
            meta = dict(description, project_path=str(case_root), project_version='0.8.30-test')
            (case_build / 'project_description.json').write_text(json.dumps(meta))
            return case_root, case_build, base / 'release'

        def remove_local(case_root, relative):
            path = case_root / relative
            assert path.resolve().is_relative_to(case_root.resolve()), 'test deletion escaped its intended fixture'
            path.unlink()

        def font_rejected(name, mutate, expected):
            nonlocal count
            cr, cb, destination = font_case(name); mutate(cr, cb)
            with patch.object(package.subprocess, 'run', side_effect=fake_merge):
                try: package.package_build(cr, cb, destination)
                except (ValueError, OSError) as error: assert expected in str(error), (name, error)
                else: raise AssertionError(f'font case {name} unexpectedly packaged')
            assert not destination.exists(), 'Failed font package left a release directory'
            count += 1

        def resource_bytes(cr, data):
            (cr / 'main/assets/folio_fonts.bin').write_bytes(data)

        def later(cr, cb, relative):
            stamp = (cb / 'app.bin').stat().st_mtime_ns + 2_000_000_000
            os.utime(cr / relative, ns=(stamp, stamp))

        # Independent resources may be newer than app, but no code or legacy font
        # receives this exception. Both patterns are exercised explicitly.
        cr, cb, destination = font_case('valid-newer-independent-assets')
        later(cr, cb, 'main/assets/folio_fonts.bin')
        later(cr, cb, 'main/assets/fonts/manifest.json')
        later(cr, cb, 'main/assets/fonts/NOTICE.txt')
        with patch.object(package.subprocess, 'run', side_effect=fake_merge):
            package.package_build(cr, cb, destination)
        fm = json.loads((destination / 'manifest.json').read_text())
        resources = fm['font_resources']
        assert resources['offset'] == '0x410000' and resources['size'] == '0x500000'
        assert resources['sha256'] == real_info['sha256'] and resources['bytes'] == 0x500000
        assert resources['not_default_flash'] and not resources['included_in_merged'] and not resources['included_in_ota']
        assert resources['requires_complete_storage_backup'] and resources['requires_explicit_overwrite_permission']
        assert not resources['device_verified']
        assert (destination / 'font-resources.bin').read_bytes() == real_image
        for relative, raw in readable.items(): assert (destination / 'font-resources' / relative).read_bytes() == raw
        assert fm['flash_files'] == manifest['flash_files'] == fm['usb_install_files']
        assert fm['optional_merged_install_files'] == manifest['optional_merged_install_files']
        assert fm['merged_components'] == ['bootloader.bin', 'partition-table.bin', 'app.bin']
        assert (destination / 'merged.bin').stat().st_size < 0x410000
        assert sorted(p.name for p in (destination / 'update').iterdir()) == ['folio.bin', 'folio.bin.sha256']
        assert (destination / 'update/folio.bin').read_bytes() == (cb / 'app.bin').read_bytes()
        usb = (destination / 'usb-install.txt').read_text()
        for line in usb.splitlines():
            if line.startswith('python -m esptool'):
                assert 'font' not in line and '0x410000' not in line and '0x500000' not in line
        guide = (destination / 'font-resources-install.txt').read_text()
        for text in ('ENTIRE', 'explicit user permission', '0x410000..0x90FFFF', 'Do not change partitions',
                     'NVS', 'otadata', 'TF-card', 'Read back', real_info['sha256']):
            assert text in guide, text
        assert 'write-flash' not in guide and 'python -m esptool' not in guide
        for relative, expected in fm['sha256'].items(): assert package.digest(destination / relative) == expected
        assert 'font-resources.bin' in fm['sha256'] and 'font-resources/NOTICE.txt' in fm['sha256']
        assert not (cr / 'artifacts').exists()
        count += 1

        font_rejected('missing-resource', lambda cr, cb: remove_local(cr, 'main/assets/folio_fonts.bin'), 'No such file')
        font_rejected('bad-resource', lambda cr, cb: resource_bytes(cr, b'X' * 0x500000), 'magic')
        font_rejected('truncated-resource', lambda cr, cb: resource_bytes(cr, real_image[:-1]), 'exactly 5MiB')
        font_rejected('oversized-resource', lambda cr, cb: resource_bytes(cr, real_image + b'\xff'), 'exactly 5MiB')
        def corrupt_sha(cr, cb):
            bad = bytearray(real_image); bad[64 + 40] ^= 1
            validator.put32(bad, 44, validator.header_crc(bad[:4096])); resource_bytes(cr, bad)
        font_rejected('compressed-sha', corrupt_sha, 'SHA256 mismatch')
        def corrupt_payload(cr, cb):
            bad = bytearray(real_image); bad[49152 + 20] ^= 1; resource_bytes(cr, bad)
        font_rejected('payload-crc', corrupt_payload, 'CRC32 mismatch')
        font_rejected('missing-notice', lambda cr, cb: remove_local(cr, 'main/assets/fonts/NOTICE.txt'), 'No such file')
        font_rejected('changed-notice', lambda cr, cb: (cr / 'main/assets/fonts/NOTICE.txt').write_text('changed'), 'differs')
        font_rejected('source-file-sha', lambda cr, cb: (cr / 'main/assets/fonts/source-manifest.json').write_bytes(readable['source-manifest.json'] + b'\n'), 'source-manifest SHA256')
        license_name = source_record['fonts'][0]['license_path']
        font_rejected('bad-license', lambda cr, cb: (cr / 'main/assets/fonts' / license_name).write_text('changed'), 'license SHA256')
        for name, relative in [('newer-code', 'main/app.c'), ('newer-loader', 'main/book/folio_font_pack.c'),
                               ('newer-legacy-songti', 'main/assets/folio_builtin.deflate')]:
            font_rejected(name, lambda cr, cb, relative=relative: later(cr, cb, relative), 'older than source')
        def similar_name(cr, cb):
            (cr / 'main/assets/fonts_extra.bin').write_bytes(b'not an independent font path')
            later(cr, cb, 'main/assets/fonts_extra.bin')
        font_rejected('newer-nonexcluded-asset', similar_name, 'older than source')
        external_image = temp / 'outside-font-resources.bin'; external_image.write_bytes(real_image)
        def escaped_resource(cr, cb):
            remove_local(cr, 'main/assets/folio_fonts.bin')
            (cr / 'main/assets/folio_fonts.bin').symlink_to(external_image)
        font_rejected('resource-symlink-escape', escaped_resource, 'escapes')
        external_license = temp / 'outside-license.txt'; external_license.write_bytes(readable[license_name])
        def escaped_license(cr, cb):
            remove_local(cr, 'main/assets/fonts/' + license_name)
            (cr / 'main/assets/fonts' / license_name).symlink_to(external_license)
        font_rejected('license-symlink-escape', escaped_license, 'escapes')
        def malicious_license_path(cr, cb):
            # Keep every pack CRC/SHA and external metadata consistent so this
            # probes path validation, not an earlier integrity rejection.
            meta = validator.load_json(real_image[real_info['metadata_offset']:real_info['metadata_offset'] + real_info['metadata_bytes']])
            meta['manifest']['fonts'][0]['license_path'] = 'licenses/../../outside-license.txt'
            meta['sources']['fonts'][0]['license_path'] = 'licenses/../../outside-license.txt'
            source = validator.json_bytes(meta['sources'])
            meta['manifest']['source_manifest_sha256'] = hashlib.sha256(source).hexdigest()
            (cr / 'main/assets/fonts/source-manifest.json').write_bytes(source)
            (cr / 'main/assets/fonts/manifest.json').write_bytes(validator.json_bytes(meta['manifest']))
            encoded = validator.json_bytes(meta, compact=True)
            bad = bytearray(real_image); bad[4096:49152] = b'\xff' * (49152 - 4096)
            bad[4096:4096 + len(encoded)] = encoded
            validator.put32(bad, 36, len(encoded)); validator.put32(bad, 40, validator.crc(encoded))
            validator.put32(bad, 44, validator.header_crc(bad[:4096])); resource_bytes(cr, bad)
        font_rejected('license-path-traversal', malicious_license_path, 'escapes')
        for relative in ('../outside-font-resources.bin', str(external_image)):
            try: package.source_file(root, relative)
            except ValueError as error: assert 'escape' in str(error)
            else: raise AssertionError('Unsafe resource source path accepted')
            count += 1
        assert package.FLASH_NAMES == {0: 'bootloader.bin', 0x8000: 'partition-table.bin', 0x10000: 'app.bin', 0x910000: 'ota_data_initial.bin'}
        print(f'Packaging: {count} release/layout/resource/safety cases passed; independent fonts never enter default USB/merged/OTA; no device access or artifacts dependency')


if __name__ == '__main__': main()
