import { describe, it, expect, vi, afterEach } from 'vitest';
import { md5 } from '@noble/hashes/legacy.js';
import { readStubFlash, browserSupport } from '../src/installer/serial';
import { fetchBounded } from '../src/installer/manifest';

function protocol(bytes: Uint8Array, options: { digest?: Uint8Array; split?: number; extra?: boolean; splitDigest?: boolean } = {}) {
  const frames: Uint8Array[] = [];
  const step = options.split && options.split > 0 ? options.split : 512;
  for (let i = 0; i < bytes.length; i += step) frames.push(bytes.slice(i, i + step));
  if (options.extra) frames[0] = new Uint8Array(frames[0].length + 1);
  const digest = options.digest ?? md5(bytes);
  if (options.splitDigest && !options.digest) { frames.push(digest.slice(0, 7), digest.slice(7)); }
  else frames.push(digest);
  const acknowledgements: number[] = [];
  const requests: Uint8Array[] = [];
  const loader = { IS_STUB: true, ESP_READ_FLASH: 0xd2, FLASH_READ_TIMEOUT: 3000,
    async checkCommand(_description: string, op: number, request: Uint8Array) { expect(op).toBe(0xd2); requests.push(request); return 0; },
    transport: { async read() { const frame = frames.shift(); if (!frame) throw Error('Missing frame'); return frame; },
      async write(bytes: Uint8Array) { acknowledgements.push(new DataView(bytes.buffer).getUint32(0, true)); } } };
  return { loader, frames, acknowledgements, requests };
}
describe('stub serial read protocol (no actual serial port)', () => {
  it('reads bounded data with exact ACKs and consumes/verifies terminal MD5', async () => {
    const bytes = new Uint8Array(9000).fill(0x42), p = protocol(bytes), progress: number[] = [];
    const output = await readStubFlash(p.loader, 0, bytes.length, new AbortController().signal, n => progress.push(n));
    expect(output).toEqual(bytes); expect(p.frames).toEqual([]); expect(progress).toEqual(p.acknowledgements);
    expect(p.acknowledgements).toEqual(Array.from({ length: 18 }, (_, index) => Math.min(512 * (index + 1), 9000)));
    const view = new DataView(p.requests[0].buffer); expect([0, 4, 8, 12].map(n => view.getUint32(n, true))).toEqual([0, 9000, 512, 1]);
  });
  it('reads the complete 16 MiB backup with all frame ACKs and its final digest', async () => {
    const size = 0x1000000, data = new Uint8Array(size);
    for (let at = 0; at < size; at++) data[at] = (at >>> 16) ^ (at >>> 8) ^ at;
    const digest = md5(data); let readAt = 0, ack = 0, finalDigestRead = false;
    const p = protocol(new Uint8Array());
    p.loader.transport.read = async () => {
      if (readAt === size) { finalDigestRead = true; return digest; }
      if (readAt !== ack) throw new Error('Missing ACK');
      const frame = data.subarray(readAt, readAt + 512); readAt += frame.length; return frame;
    };
    p.loader.transport.write = async bytes => { ack = new DataView(bytes.buffer).getUint32(0, true); };
    const output = await readStubFlash(p.loader, 0, size, new AbortController().signal, () => {});
    expect(ack).toBe(size); expect(finalDigestRead).toBe(true); expect(output.length).toBe(size);
    expect(md5(output)).toEqual(digest);
  });
  it('stops a stalled full backup after three empty frames without ACKs or progress', async () => {
    const p = protocol(new Uint8Array(512));
    const read = vi.spyOn(p.loader.transport, 'read').mockResolvedValue(new Uint8Array());
    const progress = vi.fn();
    await expect(readStubFlash(p.loader, 0, 0x1000000, new AbortController().signal, progress)).rejects.toThrow('空帧');
    expect(read).toHaveBeenCalledTimes(3); expect(progress).not.toHaveBeenCalled(); expect(p.acknowledgements).toEqual([]);
  });
  it('rejects a short data frame instead of treating corruption as a fragment', async () => {
    const bytes = new Uint8Array(9000).fill(0x5a), p = protocol(bytes, { split: 1000 });
    await expect(readStubFlash(p.loader, 0, bytes.length, new AbortController().signal, () => {})).rejects.toThrow('长度不符');
    expect(p.acknowledgements).toEqual([]);
  });
  it.each([{ digest: new Uint8Array(16) }, { digest: new Uint8Array(15) }, { extra: true }])('rejects corrupted or malformed read stream %j', async options => {
    const p = protocol(new Uint8Array(4096).fill(1), options);
    await expect(readStubFlash(p.loader, 0, 4096, new AbortController().signal, () => {})).rejects.toThrow();
  });
  it('respects cancellation and rejects ROM reads or out-of-range addresses', async () => {
    const p = protocol(new Uint8Array(4096)); const abort = new AbortController(); abort.abort();
    await expect(readStubFlash(p.loader, 0, 4096, abort.signal, () => {})).rejects.toThrow();
    expect(p.requests).toHaveLength(0); p.loader.IS_STUB = false;
    await expect(readStubFlash(p.loader, 0, 4096, new AbortController().signal, () => {})).rejects.toThrow();
    p.loader.IS_STUB = true;
    await expect(readStubFlash(p.loader, 0x1000000, 4096, new AbortController().signal, () => {})).rejects.toThrow();
  });
});
afterEach(() => vi.unstubAllGlobals());
describe('network isolation and browser gates', () => {
  it('fetches without redirects, credentials, cache or referrer; bounds streamed bytes', async () => {
    const url = 'https://qmqsu.github.io/folio/firmware/v/image.bin';
    const mock = vi.fn(async () => { const response = new Response(new Uint8Array([1, 2, 3])); Object.defineProperty(response, 'url', { value: url }); return response; });
    vi.stubGlobal('fetch', mock);
    expect(await fetchBounded(url, 3)).toEqual(new Uint8Array([1, 2, 3]));
    expect(mock.mock.calls[0]).toEqual([url, expect.objectContaining({ redirect: 'error', credentials: 'omit', referrerPolicy: 'no-referrer', cache: 'no-store' })]);
    await expect(fetchBounded(url, 2)).rejects.toThrow('超过');
  });
  it('404 is unpublished, never a fabricated version', async () => {
    vi.stubGlobal('fetch', async () => new Response(null, { status: 404 }));
    await expect(fetchBounded('https://qmqsu.github.io/folio/channels/stable-usb.json', 2000)).rejects.toThrow('未发布');
  });
  it.each([
    ['Mozilla/5.0 Chrome/130.0', true, true, true], ['Mozilla/5.0 Edg/130.0 Chrome/130.0', true, true, true],
    ['Mozilla/5.0 Safari/605.1', true, true, false], ['Mozilla/5.0 Firefox/130.0', true, true, false],
    ['Mozilla/5.0 Android Chrome/130.0', true, true, false], ['Mozilla/5.0 Chrome/130.0', false, true, false],
    ['Mozilla/5.0 Chrome/130.0', true, false, false],
  ])('detects browser %s / secure %s / serial %s', (userAgent, secure, serial, supported) => {
    vi.stubGlobal('isSecureContext', secure); vi.stubGlobal('navigator', { userAgent, platform: 'MacIntel', maxTouchPoints: 0, ...(serial ? { serial: {} } : {}) });
    expect(browserSupport().supported).toBe(supported);
  });
});
