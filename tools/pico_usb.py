#!/usr/bin/env python3
"""Physical USB development control. Does not reset, flash, or open network ports."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import secrets
import select
import struct
import termios
import time
import tty
import zlib


def checksum(data):
    result = 2166136261
    for byte in data:
        result = ((result ^ byte) * 16777619) & 0xffffffff
    return result


def decode_runs(data, expected):
    if len(data) % 2:
        raise ValueError('Truncated screenshot run')
    out = bytearray()
    for i in range(0, len(data), 2):
        count, value = data[i:i+2]
        if not count or len(out) + count > expected:
            raise ValueError('Invalid screenshot run')
        out.extend(bytes([value]) * count)
    if len(out) != expected:
        raise ValueError('Incomplete screenshot')
    return out


def save_screen(path, packed, width, height):
    # Device uses EPD_ROT_INVERTED_PORTRAIT, even physical pixels in low nibble.
    pixels = bytearray()
    for y in range(width):
        pixels.append(0)  # PNG row filter
        for x in range(height):
            byte = packed[(height - 1 - x) * (width // 2) + y // 2]
            pixels.append(((byte >> (4 * (y & 1))) & 15) * 17)
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', height, width, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')
    Path(path).write_bytes(png)


def run(args):
    command_id = secrets.randbelow(999999999) + 1
    command = args.command.upper()
    if command == 'TAP':
        if not 0 <= args.x < 684 or not 0 <= args.y < 1216:
            raise ValueError('Touch is outside the screen')
        command += f' {args.x} {args.y}'
    elif command == 'KEY':
        command += f' {args.key}'
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    log = None
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        tty.setraw(fd, termios.TCSANOW)
        settings = termios.tcgetattr(fd)
        settings[2] = (settings[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        if args.log:
            log = open(args.log, 'ab')
        if command != 'WATCH':
            os.write(fd, f'@FOLIO {command_id} {command}\n'.encode('ascii'))
        deadline = time.monotonic() + args.timeout
        buffer = bytearray()
        encoded = bytearray()
        dimensions = None
        while time.monotonic() < deadline:
            if not select.select([fd], [], [], .2)[0]:
                continue
            try:
                data = os.read(fd, 65536)
            except BlockingIOError:
                continue
            if not data:
                raise RuntimeError('USB disconnected')
            buffer.extend(data)
            if len(buffer) > 131072:
                raise ValueError('Oversized serial line')
            while b'\n' in buffer:
                line, _, remaining = buffer.partition(b'\n')
                buffer = bytearray(remaining)
                line = line.rstrip(b'\r')
                marker = f'@FOLIO {command_id} '.encode('ascii')
                if marker not in line:
                    if log and b'@FOLIO ' not in line:
                        log.write(line + b'\n');log.flush()
                    continue
                text = line.split(marker, 1)[1].decode('utf-8')
                kind, _, value = text.partition(' ')
                if kind == 'ERROR':
                    raise RuntimeError(f'Device: {value}')
                if kind == 'STATUS':
                    print(json.dumps(json.loads(value), ensure_ascii=False));return
                if kind == 'OK':
                    print(json.dumps({'ok': True, 'command': command}));return
                if kind == 'BEGIN':
                    width, height, size = map(int, value.split())
                    if (width, height, size) != (1216, 684, 415872):
                        raise ValueError('Unexpected framebuffer dimensions')
                    dimensions = (width, height, size)
                elif kind == 'DATA':
                    offset, payload = value.split()
                    if not dimensions or int(offset) != len(encoded):
                        raise ValueError('Screenshot chunk missing or reordered')
                    encoded.extend(bytes.fromhex(payload))
                    if len(encoded) > dimensions[2] * 2:
                        raise ValueError('Oversized screenshot')
                elif kind == 'END':
                    size, digest = value.split()
                    if not dimensions or int(size) != len(encoded):
                        raise ValueError('Incomplete screenshot stream')
                    packed = decode_runs(encoded, dimensions[2])
                    if checksum(packed) != int(digest, 16):
                        raise ValueError('Screenshot checksum mismatch')
                    output = Path(args.output).resolve()
                    save_screen(output, packed, *dimensions[:2])
                    print(json.dumps({'image': str(output), 'width': 684, 'height': 1216}));return
        if command == 'WATCH':
            print(json.dumps({'captured_seconds': args.timeout}));return
        raise TimeoutError('No complete USB response; device must be awake with the debug build installed')
    finally:
        if log:
            log.close()
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/cu.usbmodem2101')
    parser.add_argument('--timeout', type=float, default=45)
    parser.add_argument('--log', help='Append normal diagnostics; screenshot data is excluded')
    subs = parser.add_subparsers(dest='command', required=True)
    subs.add_parser('status');subs.add_parser('cancel');subs.add_parser('watch')
    screen = subs.add_parser('screen');screen.add_argument('output')
    tap = subs.add_parser('tap');tap.add_argument('x', type=int);tap.add_argument('y', type=int)
    key = subs.add_parser('key');key.add_argument('key', type=int, choices=range(3))
    args = parser.parse_args()
    if not 0 < args.timeout <= 1800:
        parser.error('Timeout must be within 0–1800 seconds')
    run(args)


if __name__ == '__main__':
    main()
