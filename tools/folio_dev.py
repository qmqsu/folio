#!/usr/bin/env python3
"""Device-side development loop for 页间 Folio.

Owns the USB serial port for one whole session, so logs keep flowing while
commands run. Talks the @FOLIO debug protocol implemented by
main/folio_debug.inc and reuses the framing helpers from tools/pico_usb.py.

Commands never reset or flash. Debug-only preview check, download and install
require separate explicit preview-* commands; none advances automatically.

Usage:
    python3 tools/folio_dev.py status
    python3 tools/folio_dev.py shot /tmp/home.png
    python3 tools/folio_dev.py tap 342 600
    python3 tools/folio_dev.py key 1
    python3 tools/folio_dev.py preview-check
    python3 tools/folio_dev.py preview-download
    python3 tools/folio_dev.py preview-install
    python3 tools/folio_dev.py wait --view READING --timeout 8
    python3 tools/folio_dev.py scenario tools/scenarios/smoke.json
    python3 tools/folio_dev.py diff a.png b.png
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import secrets
import select
import struct
import sys
import termios
import time
import tty
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pico_usb import checksum, decode_runs, save_screen  # noqa: E402

# Order of view_t in main/app_main.c. Keep in sync when views are added.
VIEWS = [
    'LIBRARY', 'READING', 'TOOLS', 'TOC', 'BOOKMARKS',
    'JUMP', 'SETTINGS', 'FONTS', 'SYSTEM', 'ERROR', 'OFF_CONFIRM',
    'BOOKMARK_DELETE', 'HOME', 'HOME_SETTINGS', 'CATALOG',
    'DEVICE', 'TRANSFER', 'STORAGE', 'FILES', 'FILE_ACTION',
    'DELETE_CONFIRM', 'FORMAT_CONFIRM', 'FORMAT_FINAL', 'UPDATE', 'WEREAD',
    'WIFI', 'WIFI_EDIT', 'TIME', 'TIME_EDIT', 'LOCK_PREVIEW', 'WALLPAPERS',
]
FRAMEBUFFER_BYTES = 1216 * 684 // 2


def view_name(index):
    return VIEWS[index] if 0 <= index < len(VIEWS) else f'VIEW_{index}'


class Busy(RuntimeError):
    pass


class Device:
    """Single owner of the USB console. Buffers diagnostics between commands."""

    def __init__(self, port, log_path=None, timeout=45):
        self.port = port
        self.timeout = timeout
        self.log_path = Path(log_path) if log_path else None
        self.serial = bytearray()
        self.lines = 0
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            os.close(self.fd)
            raise Busy(f'{port} is busy (another monitor or tool owns it): {error}')
        tty.setraw(self.fd, termios.TCSANOW)
        settings = termios.tcgetattr(self.fd)
        settings[2] = (settings[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
        termios.tcsetattr(self.fd, termios.TCSANOW, settings)
        if self.log_path:
            self.log_path.parent.mkdir(parents=True, exist_ok=True)

    # -- plumbing ---------------------------------------------------------
    def close(self):
        fd = self.fd
        if fd is None:
            return
        self.fd = None  # A caller's finally may close again after disconnection.
        try:
            termios.tcflush(fd, termios.TCIOFLUSH)
        except (OSError, termios.error):
            pass
        finally:
            os.close(fd)

    def _pump(self, deadline):
        """Read whatever is available until deadline; yield ('line', text)."""
        if not select.select([self.fd], [], [], max(0.0, min(0.05, deadline - time.monotonic())))[0]:
            return
        try:
            data = os.read(self.fd, 65536)
        except BlockingIOError:
            return
        if not data:
            raise RuntimeError('USB disconnected')
        self.serial.extend(data)
        if len(self.serial) > 262144:
            raise ValueError('Oversized serial stream')
        while b'\n' in self.serial:
            line, _, rest = self.serial.partition(b'\n')
            self.serial = bytearray(rest)
            line = line.rstrip(b'\r')
            if b'@FOLIO ' in line:
                yield ('frame', line)
            else:
                self._record(line)
                yield ('log', line)

    def _record(self, line):
        self.lines += 1
        if self.log_path:
            with open(self.log_path, 'ab') as handle:
                handle.write(line + b'\n')

    def command(self, name, timeout=None):
        """Send one command, return (kind, payload). Also drains diagnostics."""
        limit = self.timeout if timeout is None else timeout
        command_id = secrets.randbelow(999998) + 1
        marker = f'@FOLIO {command_id} '.encode('ascii')
        os.write(self.fd, f'@FOLIO {command_id} {name}\n'.encode('ascii'))
        deadline = time.monotonic() + limit
        encoded = bytearray()
        dimensions = None
        while time.monotonic() < deadline:
            for kind, line in self._pump(deadline):
                if kind == 'log':
                    continue
                if marker not in line:
                    continue  # reply to an earlier, abandoned command
                kind, _, value = line.split(marker, 1)[1].decode('utf-8').partition(' ')
                if kind == 'ERROR':
                    raise RuntimeError(f'device: {value}')
                if kind in ('OK', 'STATUS'):
                    payload = json.loads(value) if value else {}
                    if kind == 'STATUS':
                        payload['view_name'] = view_name(payload['view'])
                    return kind, payload
                if kind == 'BEGIN':
                    width, height, size = map(int, value.split())
                    if size != FRAMEBUFFER_BYTES:
                        raise ValueError('Unexpected framebuffer size')
                    dimensions = (width, height, size)
                elif kind == 'DATA':
                    offset, payload = value.split()
                    if not dimensions or int(offset) != len(encoded):
                        raise ValueError('Screenshot chunk missing or reordered')
                    encoded.extend(bytes.fromhex(payload))
                elif kind == 'END':
                    size, digest = value.split()
                    if not dimensions or int(size) != len(encoded):
                        raise ValueError('Incomplete screenshot stream')
                    packed = decode_runs(encoded, dimensions[2])
                    if checksum(packed) != int(digest, 16):
                        raise ValueError('Screenshot checksum mismatch')
                    return 'SCREEN', packed
            time.sleep(0.005)
        raise TimeoutError(
            'no reply; the device must be awake and running a build with '
            'CONFIG_FOLIO_USB_DEBUG')

    # -- verbs ------------------------------------------------------------
    def status(self):
        # ESP diagnostic output can interrupt a STATUS line during Wi-Fi I/O.
        for attempt in range(3):
            try:
                return self.command('STATUS')[1]
            except (json.JSONDecodeError, UnicodeDecodeError):
                if attempt == 2:
                    raise

    def import_asset(self, path):
        """Import a font or wallpaper while the device is on the library screen."""
        path = Path(path)
        name = path.name.encode('utf-8')
        if (not name or len(name) > 63 or any(c <= 32 for c in name)
                or path.suffix.lower() not in {'.ttf', '.png', '.jpg', '.jpeg'}):
            raise ValueError('Use a TTF, PNG or JPEG basename without spaces (up to 63 UTF-8 bytes)')
        length = path.stat().st_size
        if not 0 < length <= 32 * 1024 * 1024:
            raise ValueError('Expected 1 byte to 32 MiB')
        data = path.read_bytes()
        if len(data) != length:
            raise ValueError('Source file changed while reading')
        marker = f'@FOLIO {secrets.randbelow(999998) + 1} '.encode()
        os.write(self.fd, marker + f'IMPORT {length} {zlib.crc32(data):08x} '.encode() + name + b'\n')
        sent = 0
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            for kind, line in self._pump(deadline):
                if kind != 'frame' or marker not in line:
                    continue
                fields = line.split(marker, 1)[1].split()
                if fields == [b'OK'] and sent == length:
                    return {'name': path.name, 'bytes': sent}
                if len(fields) != 3 or fields[0] != b'READY':
                    raise RuntimeError(line.decode('utf-8', errors='replace'))
                offset, count = map(int, fields[1:])
                if offset != sent or not 0 < count <= min(4096, length - offset):
                    raise ValueError('Unexpected import offset or block length')
                end = offset + count
                while sent < end:
                    if not select.select([], [self.fd], [], 3)[1]:
                        raise TimeoutError('USB write timed out')
                    try:
                        sent += os.write(self.fd, data[sent:end])
                    except BlockingIOError:
                        continue
                deadline = time.monotonic() + (max(300, self.timeout) if sent == length else self.timeout)
        raise TimeoutError(f'No import acknowledgement after {sent} bytes')

    def screen(self, path=None):
        _, packed = self.command('SCREEN', timeout=max(self.timeout, 60))
        target = Path(path) if path else Path('artifacts/live') / f'screen-{stamp()}.png'
        target.parent.mkdir(parents=True, exist_ok=True)
        save_screen(target, packed, 1216, 684)
        return target

    def tap(self, x, y):
        if not (0 <= x < 684 and 0 <= y < 1216):
            raise ValueError('tap outside 684x1216')
        return self.command(f'TAP {x} {y}')[1]

    def key(self, index):
        if index not in (0, 1, 2):
            raise ValueError('key must be 0..2')
        return self.command(f'KEY {index}')[1]

    def preview_check(self):
        """Start a debug-only signed manifest check; never download or install."""
        return self.command('PREVIEW_CHECK')[1]

    def preview_download(self):
        """Explicitly download the checked preview candidate; never install."""
        return self.command('PREVIEW_DOWNLOAD')[1]

    def preview_install(self):
        """Explicitly confirm installation of an already verified preview READY file."""
        return self.command('PREVIEW_INSTALL')[1]

    def wait(self, predicate, timeout=10.0, interval=0.15):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.status()
            if predicate(last):
                return last
            time.sleep(interval)
        raise TimeoutError(f'condition not met in {timeout}s; last status={last}')


def stamp():
    return time.strftime('%Y%m%d-%H%M%S')


# -- golden image comparison ---------------------------------------------
def read_png(path):
    """Decode the filter-0 8-bit grayscale PNGs written by this toolchain."""
    data = Path(path).read_bytes()
    if not data.startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError(f'{path}: not a PNG')
    position, width, height, idat = 8, None, None, bytearray()
    while position < len(data):
        length = struct.unpack('>I', data[position:position + 4])[0]
        kind = data[position + 4:position + 8]
        body = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b'IHDR':
            width, height, depth, color = struct.unpack('>IIBB', body[:10])
            if (depth, color) != (8, 0):
                raise ValueError(f'{path}: expected 8-bit grayscale, got {depth}/{color}')
        elif kind == b'IDAT':
            idat.extend(body)
    raw = zlib.decompress(bytes(idat))
    stride = width + 1
    if len(raw) != stride * height:
        raise ValueError(f'{path}: unexpected pixel payload')
    rows = []
    for y in range(height):
        row = raw[y * stride:(y + 1) * stride]
        if row[0] != 0:
            raise ValueError(f'{path}: only filter 0 rows are supported')
        rows.append(row[1:])
    return width, height, rows


def diff_images(left, right, output=None, threshold=96):
    w1, h1, a = read_png(left)
    w2, h2, b = read_png(right)
    if (w1, h1) != (w2, h2):
        raise ValueError(f'size differs: {w1}x{h1} vs {w2}x{h2}')
    changed = 0
    box = [w1, h1, -1, -1]
    worst = 0
    out = [bytearray(b'\xff' * w1) for _ in range(h1)]
    for y in range(h1):
        row_a, row_b, row_out = a[y], b[y], out[y]
        for x in range(w1):
            delta = abs(row_a[x] - row_b[x])
            if delta >= threshold:
                changed += 1
                worst = max(worst, delta)
                if x < box[0]: box[0] = x
                if y < box[1]: box[1] = y
                if x > box[2]: box[2] = x
                if y > box[3]: box[3] = y
                row_out[x] = 0x00
            elif delta:
                row_out[x] = 0xc0
    report = {
        'width': w1, 'height': h1,
        'changed_pixels': changed,
        'changed_ratio': round(changed / (w1 * h1), 6),
        'max_delta': worst,
        'box': box if changed else None,
    }
    if output:
        write_png(output, w1, h1, [bytes(row) for row in out])
        report['diff_image'] = str(Path(output).resolve())
    return report


def write_png(path, width, height, rows):
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))

    raw = bytearray()
    for row in rows:
        raw.append(0)
        raw.extend(row)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b'')
    Path(path).write_bytes(png)


# -- scenarios ------------------------------------------------------------
def run_scenario(device, path, output_root):
    spec = json.loads(Path(path).read_text())
    run_dir = Path(output_root) / f'{stamp()}-{spec.get("name", "scenario")}'
    run_dir.mkdir(parents=True, exist_ok=True)
    device.log_path = run_dir / 'device.log'
    trace = []
    for index, step in enumerate(spec['steps'], 1):
        (action, value), = step.items()
        entry = {'step': index, 'action': action, 'value': value}
        started = time.monotonic()
        if action == 'tap':
            device.tap(*value)
        elif action == 'key':
            device.key(value)
        elif action == 'shot':
            entry['file'] = str(device.screen(run_dir / f'{value}.png'))
            entry['status'] = device.status()
        elif action == 'wait':
            want = value.get('view')
            avoid = value.get('not_view')
            text = value.get('message_contains')
            status = device.wait(
                lambda s: (want is None or s['view_name'] == want)
                and (avoid is None or s['view_name'] != avoid)
                and (text is None or text in s.get('message', '')),
                timeout=value.get('timeout', 10))
            entry['status'] = status
        elif action == 'sleep':
            time.sleep(value / 1000.0)
        elif action == 'note':
            entry['note'] = value
        else:
            raise ValueError(f'unknown step {action}')
        entry['seconds'] = round(time.monotonic() - started, 2)
        trace.append(entry)
        print(json.dumps(entry, ensure_ascii=False))
    (run_dir / 'trace.json').write_text(json.dumps(trace, ensure_ascii=False, indent=2))
    return run_dir


# -- command line ---------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', default='/dev/cu.usbmodem2101')
    parser.add_argument('--timeout', type=float, default=45)
    parser.add_argument('--log', help='append diagnostics to this file for the whole session')
    parser.add_argument('--settle', type=float, default=0,
                        help='extra seconds to wait for asynchronous work after a step')
    subs = parser.add_subparsers(dest='command', required=True)
    subs.add_parser('status')
    shot = subs.add_parser('shot')
    shot.add_argument('output', nargs='?')
    asset = subs.add_parser('import-asset', help='debug builds only: import a font or wallpaper from LIBRARY')
    asset.add_argument('path')
    tap = subs.add_parser('tap')
    tap.add_argument('x', type=int)
    tap.add_argument('y', type=int)
    key = subs.add_parser('key')
    key.add_argument('index', type=int, choices=range(3))
    subs.add_parser(
        'preview-check',
        help='debug builds only: from UPDATE, check preview without downloading or installing')
    subs.add_parser(
        'preview-download',
        help='debug builds only: explicitly download a preview AVAILABLE candidate')
    subs.add_parser(
        'preview-install',
        help='debug builds only: explicitly install a verified preview READY candidate')
    wait = subs.add_parser('wait')
    wait.add_argument('--view')
    wait.add_argument('--not-view')
    wait.add_argument('--message-contains')
    wait.add_argument('--timeout', type=float, default=10)
    run = subs.add_parser('scenario')
    run.add_argument('path')
    run.add_argument('--out', default='artifacts/live')
    diff = subs.add_parser('diff')
    diff.add_argument('left')
    diff.add_argument('right')
    diff.add_argument('--out', help='write a highlighted diff PNG here')
    diff.add_argument('--threshold', type=int, default=96)
    args = parser.parse_args()

    if args.command == 'diff':
        print(json.dumps(diff_images(args.left, args.right, args.out, args.threshold)))
        return 0

    device = Device(args.port, args.log, args.timeout)
    try:
        if args.command == 'status':
            print(json.dumps(device.status(), ensure_ascii=False))
        elif args.command == 'shot':
            path = device.screen(args.output)
            print(json.dumps({'image': str(path), 'width': 684, 'height': 1216}))
        elif args.command == 'import-asset':
            print(json.dumps(device.import_asset(args.path), ensure_ascii=False))
        elif args.command == 'tap':
            print(json.dumps(device.tap(args.x, args.y)))
        elif args.command == 'key':
            print(json.dumps(device.key(args.index)))
        elif args.command == 'preview-check':
            print(json.dumps(device.preview_check(), ensure_ascii=False))
        elif args.command == 'preview-download':
            print(json.dumps(device.preview_download(), ensure_ascii=False))
        elif args.command == 'preview-install':
            print(json.dumps(device.preview_install(), ensure_ascii=False))
        elif args.command == 'wait':
            status = device.wait(
                lambda s: (args.view is None or s['view_name'] == args.view)
                and (args.not_view is None or s['view_name'] != args.not_view)
                and (args.message_contains is None or args.message_contains in s.get('message', '')),
                timeout=args.timeout)
            print(json.dumps(status, ensure_ascii=False))
        elif args.command == 'scenario':
            run_dir = run_scenario(device, args.path, args.out)
            print(json.dumps({'run_dir': str(run_dir), 'log_lines': device.lines}))
        if args.settle:
            time.sleep(args.settle)
        return 0
    except Busy as error:
        print(json.dumps({'error': str(error)}), file=sys.stderr)
        return 2
    finally:
        device.close()


if __name__ == '__main__':
    sys.exit(main())
