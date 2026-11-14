#!/usr/bin/env python3
"""Run real storage C code against isolated cards; hash every original payload."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
# Opt-in evidence directory; never overwrite a previous release's artifacts.
OUTPUT = Path(os.environ['FOLIO_STORAGE_MIGRATION_OUT']) if os.environ.get('FOLIO_STORAGE_MIGRATION_OUT') else None


def checksum(data):
    value = 2166136261
    for b in data:
        value = ((value ^ b) * 16777619) & 0xffffffff
    return value


def old_record(name='selected.png', generation=5):
    body = struct.pack('<II256s', 0x31505746, generation, name.encode())
    return body + struct.pack('<I', checksum(body))


def canonical_record(name='selected.png', generation=5):
    body = struct.pack('<II256sII4I', 0x32505746, generation, name.encode(), 1, 0, 0, 0, 0, 0)
    return body + struct.pack('<I', checksum(body))


def main():
    compiler = os.environ.get('CC') or '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    if not Path(compiler).is_file():
        compiler = shutil.which('clang') or shutil.which('cc')
    sdk = Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
    results = []
    with tempfile.TemporaryDirectory(prefix='folio-storage-migration-') as temporary:
        temp = Path(temporary).resolve()
        sd = temp / 'sd'
        outside = temp / 'outside'
        outside.mkdir()
        (outside / 'sentinel.txt').write_bytes(b'OUTSIDE MUST SURVIVE\x00\xff')
        outside_hash = hashlib.sha256((outside / 'sentinel.txt').read_bytes()).hexdigest()
        binary = temp / 'migration-tests'
        command = [compiler, *(['-isysroot', str(sdk)] if sdk.is_dir() else []), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                   '-Wall', '-Wextra', '-Werror', '-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   f'-DFOLIO_SD_ROOT="{sd}"', '-I', str(ROOT / 'main'), str(ROOT / 'main/folio_storage.c'),
                   str(HERE / 'test_picture_migration.c'), '-lpthread', '-o', str(binary)]
        subprocess.run(command, check=True)

        def reset():
            # Only this exact generated path is ever cleaned. Never follow a root link.
            assert sd.parent == temp and temp.name.startswith('folio-storage-migration-')
            if sd.is_symlink():
                assert sd.lstat()
                sd.unlink()
            elif sd.exists():
                assert sd.resolve() == temp / 'sd'
                if sd.is_dir():
                    shutil.rmtree(sd)
                else:
                    sd.unlink()
            sd.mkdir()
            write('books/书籍 原名.epub', b'BOOK-ORIGINAL\x00\x80')
            write('fonts/原字体.ttf', b'FONT-ORIGINAL')
            write('.folio/state', b'STATE-ORIGINAL')

        def write(name, content):
            path = sd / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

        def legacy(selected=True):
            write('wallpapers/selected.png', b'ORIGINAL SELECTED IMAGE\x00\xff')
            write('wallpapers/原名 图.png', b'OTHER LEGACY IMAGE')
            write('wallpapers/子目录/保留.txt', b'NOT AN IMAGE; DO NOT DROP')
            write('wallpapers/.hidden.png', b'HIDDEN; DO NOT DROP')
            if selected:
                write('.folio/wallpaper.a', old_record())

        def snapshot():
            result = {}
            if sd.is_dir() and not sd.is_symlink():
                for directory, dirs, files in os.walk(sd, followlinks=False):
                    dirs[:] = [d for d in dirs if not (Path(directory) / d).is_symlink()]
                    for name in files:
                        path = Path(directory) / name
                        relative = path.relative_to(sd).as_posix()
                        if path.is_symlink() or relative in ('.folio/wallpaper.a', '.folio/wallpaper.b'):
                            continue
                        if path.is_file():
                            result[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
            return result

        def verify(before, renamed=False):
            after = snapshot()
            for name, digest in before.items():
                target = 'picture/' + name[len('wallpapers/'):] if renamed and name.startswith('wallpapers/') else name
                assert after.get(target) == digest, (name, target, after)
            assert hashlib.sha256((outside / 'sentinel.txt').read_bytes()).hexdigest() == outside_hash
            assert [p.name for p in outside.iterdir()] == ['sentinel.txt']

        def run(mode, *args, expected=0):
            completed = subprocess.run([str(binary), mode, *map(str, args)], check=False, timeout=30,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if completed.returncode != expected:
                print(completed.stdout, flush=True)
                raise AssertionError((mode, args, completed.returncode, expected))
            return completed

        def case(name, mode, renamed=False):
            before = snapshot()
            run(mode)
            verify(before, renamed)
            results.append({'case': name, 'result': 'passed', 'original_sha256': before})
            print('PASS', name, flush=True)

        reset(); case('no legacy/no picture creates only picture', 'empty')
        reset(); write('picture/原名 图.png', b'CANONICAL'); case('existing picture unchanged', 'canonical')
        reset(); legacy(); case('legacy v1 selection + nested/hidden files renamed intact', 'legacy', True)
        for size in (2 * 1024 * 1024 + 1, 32 * 1024 * 1024):
            reset(); legacy()
            with (sd / 'wallpapers/selected.png').open('r+b') as image:
                image.truncate(size)
            case(f'legacy selected {size}-byte source migrates intact; no large shutdown fallback', 'legacy-large', True)
        reset(); legacy(False); run('save-legacy'); case('directory-qualified legacy selection survives rename', 'legacy', True)
        reset(); write('wallpapers/原名 图.png', b'UNSELECTED'); case('unselected legacy renamed intact', 'legacy-unselected', True)
        reset(); legacy(); write('picture/selected.png', b'WRONG SAME BASENAME'); write('picture/z.png', b'CANONICAL Z')
        case('coexisting duplicates retained + pagination + explicit selections', 'coexist')
        reset(); write('picture/selected.png', b'WRONG SAME BASENAME'); write('.folio/wallpaper.a', old_record())
        case('v1 without legacy never silently selects picture basename', 'ambiguous')
        reset(); legacy(); write('picture/z.png', b'CANONICAL FALLBACK'); write('.folio/wallpaper.a', canonical_record())
        case('missing canonical selection falls back only within picture', 'canonical-fallback')
        reset(); legacy(); write('.folio/wallpaper.a', canonical_record())
        case('legacy rename cannot manufacture stale canonical selection by basename', 'unsafe')
        reset(); legacy(); write('.folio/wallpaper.b', b'INTERRUPTED PARTIAL RECORD')
        case('torn newest slot recovers old selected identity', 'recover', True)
        reset()
        for name in ('picture', 'wallpapers', '原名 文件夹', '自定义目录'):
            (sd / name).mkdir()
        case('file manager preserves all original folder names', 'names')

        for name in ('picture', 'wallpapers'):
            reset(); write(name, b'CONFLICTING ROOT FILE'); case(name + ' root file refused', 'unsafe')
            reset(); (sd / name).symlink_to(outside, target_is_directory=True); case(name + ' root link refused', 'unsafe')
        reset(); legacy(); (sd / 'wallpapers/link').symlink_to(outside, target_is_directory=True)
        case('legacy descendant directory link refused without moving anything', 'unsafe')
        reset(); legacy(); (sd / 'wallpapers/linked.png').symlink_to(outside / 'sentinel.txt')
        case('legacy descendant image link refused', 'unsafe')
        reset(); legacy(); (sd / 'wallpapers/bad.. ').mkdir()
        case('FAT alias/invalid descendant refused', 'unsafe')
        reset(); legacy(); (sd / '.folio/wallpaper.b').symlink_to(outside / 'sentinel.txt')
        case('migration record symlink refuses journal/rename', 'unsafe')
        reset(); legacy(); os.link(sd / 'books/书籍 原名.epub', sd / '.folio/wallpaper.b')
        case('migration record hardlink cannot truncate a user book', 'unsafe')
        reset(); legacy(); (sd / '.folio/wallpaper.b').mkdir()
        case('record target directory refused', 'unsafe')
        reset(); write('picture/selected.png', b'IMAGE'); (sd / '.folio/wallpaper.a').symlink_to(outside / 'sentinel.txt')
        case('selection save cannot follow metadata link', 'unsafe-save')
        reset(); (sd / 'picture').symlink_to(outside, target_is_directory=True)
        case('selection save cannot follow image directory link', 'unsafe-save')
        # A root symlink/file must fail before even creating picture.
        # Replace only the generated root; outside remains a fixed sentinel.
        reset(); assert sd.resolve() == temp / 'sd'; shutil.rmtree(sd); sd.symlink_to(outside, target_is_directory=True)
        run('unsafe'); assert sorted(p.name for p in outside.iterdir()) == ['sentinel.txt']
        results.append({'case': 'root symlink refused', 'result': 'passed'})
        reset(); assert sd.resolve() == temp / 'sd'; shutil.rmtree(sd); sd.write_bytes(b'ROOT FILE')
        run('unsafe'); assert sd.read_bytes() == b'ROOT FILE'
        results.append({'case': 'root regular file refused', 'result': 'passed'})

        # Kill at every cancellation boundary: before journaling, during image
        # hashing, and after fsynced journal but before rename. Recovery is a new process.
        saw_pending = False
        interrupted = 0
        pending_payload = None
        for boundary in range(80):
            reset(); legacy(); before = snapshot()
            completed = subprocess.run([str(binary), 'crash', str(boundary)], check=False, timeout=30,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            assert completed.returncode in (0, 75), completed.stdout
            if (sd / '.folio/wallpaper.b').is_file():
                pending_payload = (sd / '.folio/wallpaper.b').read_bytes()
                if (sd / 'wallpapers').is_dir():
                    saw_pending = True
            verify(before, renamed=not (sd / 'wallpapers').exists())
            run('recover'); verify(before, renamed=True)
            interrupted += completed.returncode == 75
            if completed.returncode == 0:
                break
        assert saw_pending and interrupted > 3 and pending_payload is not None
        results.append({'case': 'process interruption at all mutation/check boundaries + new-process recovery',
                        'result': 'passed', 'crash_boundaries': interrupted, 'saw_durable_pre_rename_journal': saw_pending})
        print('PASS crash/reentry boundaries:', interrupted, flush=True)
        reset(); legacy(); before = snapshot()
        run('race-directory', interrupted - 1); verify(before)
        assert list((sd / 'picture').iterdir()) == []
        results.append({'case': 'late empty destination cannot be replaced by rename', 'result': 'passed'})
        print('PASS late empty destination no-replace race', flush=True)

        reset(); legacy(); write('.folio/wallpaper.b', pending_payload); write('picture/selected.png', b'WRONG IMAGE')
        case('new picture appears after journal; selected legacy remains', 'pending-legacy')
        reset(); legacy(); run('recover'); write('wallpapers/selected.png', b'NEW WRONG LEGACY')
        case('recreated legacy cannot hide migrated original', 'pending-conflict')
        reset(); legacy(); run('recover'); write('picture/selected.png', b'CHANGED SAME BASENAME')
        case('pending identity cannot resolve changed image by basename', 'changed-pending')
        print('Migration host tests passed (ASan + UBSan); no real SD path used', flush=True)
    if OUTPUT is not None:
        OUTPUT.mkdir(parents=True, exist_ok=True)
        with (OUTPUT / 'migration-results.json').open('x') as result_file:
            result_file.write(json.dumps({'tests': results, 'total': len(results),
                'scope': 'host filesystems + process interruption; not physical FAT power-cut testing'}, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
