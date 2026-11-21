import { describe, it, expect, vi, afterEach } from 'vitest';
import { assertVerified, validatePayload, verifyEnvelope, validateImage, firmwareUrl, fetchRelease, fetchWithMirror, mirrorUrl, MAX_ENVELOPE_BYTES, PUBLIC_KEY_HEX, PUBLIC_KEY_SHA256, sha256 } from '../src/installer/manifest';
import { md5 } from '@noble/hashes/legacy.js';
import { fixture, table } from './fixtures';
import { assessInstalledLayout, validatePartitionTable } from '../src/installer/partition';

afterEach(() => vi.unstubAllGlobals());
describe('ECDSA envelope and strict USB contract', () => {
  it('accepts raw 64-byte P256 signatures over exact UTF8 bytes; freezes the result', async () => {
    const f = await fixture(); expect(f.release.kind).toBe('usb'); expect(atob(f.envelope.signature).length).toBe(64);
    expect(() => assertVerified(f.release)).not.toThrow(); expect(Object.isFrozen(f.release.parts[0])).toBe(true);
  });
  it('refuses noncanonical legacy labels even when the table digest is valid', () => {
    const bytes = table(true); bytes[16] = 1; // Hidden payload after NUL in the first label.
    bytes.set(md5(bytes.subarray(0, 4 * 32)), 4 * 32 + 16);
    expect(assessInstalledLayout(bytes).compatible).toBe(false);
  });
  it('pins the owner-provided production SEC1 fingerprint', async () => {
    expect(await sha256(Uint8Array.from(PUBLIC_KEY_HEX.match(/../g)!, s => parseInt(s, 16)))).toBe(PUBLIC_KEY_SHA256);
    const f = await fixture(); await expect(verifyEnvelope(f.envelope)).rejects.toThrow('签名');
  });
  it('rejects payload tampering and an unknown key id', async () => {
    const f = await fixture();
    const payload = btoa(atob(f.envelope.payload).replace('0.8.32-preview', '0.8.33-preview'));
    await expect(verifyEnvelope({ ...f.envelope, payload }, f.publicKey)).rejects.toThrow('签名');
    await expect(verifyEnvelope({ ...f.envelope, key_id: 'attacker' }, f.publicKey)).rejects.toThrow('未知');
    expect(() => assertVerified(validatePayload(f.payload))).toThrow('尚未');
  });
  it.each(['schema', 'kind', 'project', 'chip', 'hardware', 'layout', 'channel', 'flash_size'])('rejects signed but incompatible %s', async key => {
    const f = await fixture(); const value = { ...f.payload, [key]: key === 'kind' ? 'ota' : 'wrong' };
    await expect(verifyEnvelope(await f.sign(value), f.publicKey)).rejects.toThrow();
  });
  it.each([-1, 1.5, 0x100000000, '832', null])('rejects non-uint32 sequence %s', async sequence => {
    const f = await fixture(); expect(() => validatePayload({ ...f.payload, sequence })).toThrow();
  });
  it('rejects optional write ranges, extra fields and duplicated offsets', async () => {
    const f = await fixture();
    expect(() => validatePayload({ ...f.payload, fonts: [] })).toThrow();
    expect(() => validatePayload({ ...f.payload, parts: [...f.payload.parts, { ...f.payload.parts[0], offset: 0x410000 }] })).toThrow();
    for (const offset of [0x9000, 0xe000, 0x410000, 0x920000, 0xc20000, 0x1000000, '0x10000']) {
      expect(() => validatePayload({ ...f.payload, parts: f.payload.parts.map((p, i) => i === 2 ? { ...p, offset } : p) })).toThrow();
    }
    expect(() => validatePayload({ ...f.payload, parts: f.payload.parts.map((p, i) => i === 3 ? { ...p, offset: 0x10000 } : p) })).toThrow();
  });
  it.each([[0, 0x8001], [1, 0x1001], [1, 3073], [2, 0x300001], [3, 8191], [2, 0], [2, -1], [2, 1.5]])('rejects invalid part %d size %d', async (index, size) => {
    const f = await fixture(); expect(() => validatePayload({ ...f.payload, parts: f.payload.parts.map((p, i) => i === index ? { ...p, size } : p) })).toThrow();
  });
  it('checks every downloaded byte hash, app identity, partition and erased OTA data', async () => {
    const f = await fixture();
    for (let i = 0; i < 4; i++) await expect(validateImage(f.release.parts[i], f.images[i], f.release)).resolves.toBeUndefined();
    for (let i = 0; i < 4; i++) {
      const bytes = f.images[i].slice(); bytes[0] ^= 1;
      await expect(validateImage(f.release.parts[i], bytes, f.release)).rejects.toThrow('SHA-256');
    }
    const bytes = f.images[2].slice(); bytes[80] = 88;
    await expect(validateImage({ ...f.release.parts[2], sha256: await sha256(bytes) }, bytes, f.release)).rejects.toThrow('项目名');
    const ota = f.images[3].slice(); ota[500] = 0;
    await expect(validateImage({ ...f.release.parts[3], sha256: await sha256(ota) }, ota, f.release)).rejects.toThrow('擦除态');
  });
  it.each([
    'http://qmqsu.github.io/folio/firmware/v/folio.bin', 'https://evil.test/folio/firmware/v/folio.bin',
    'https://qmqsu.github.io/other/v/folio.bin', 'https://qmqsu.github.io/folio/firmware/v/../folio.bin',
    'https://qmqsu.github.io/folio/firmware/v/%2e%2e/folio.bin', 'https://qmqsu.github.io/folio/firmware/v/folio.bin?q=1',
    'https://qmqsu.github.io/folio/firmware/v/folio.bin#x', 'https://user@qmqsu.github.io/folio/firmware/v/folio.bin',
    '//qmqsu.github.io/folio/firmware/v/folio.bin',
  ])('rejects unsafe URL %s', url => expect(() => firmwareUrl(url)).toThrow());
  it.each([0, 1, 2, 3])('binds part %d to the signed version directory and exact filename', async index => {
    const f = await fixture();
    const original = f.payload.parts[index].url;
    for (const url of [original.replace('0.8.32-preview', '0.8.33-preview'), original.replace('/firmware/', '/firmware/nested/'), original.replace(/[^/]+$/, 'other.bin'), f.payload.parts[(index + 1) % 4].url]) {
      const payload = { ...f.payload, parts: f.payload.parts.map((p, i) => i === index ? { ...p, url } : p) };
      await expect(verifyEnvelope(await f.sign(payload), f.publicKey)).rejects.toThrow();
    }
  });
  it('accepts only optional notes within the 512-byte UTF8 limit', async () => {
    const f = await fixture();
    for (const notes of ['', 'x'.repeat(512), '中'.repeat(170) + 'ab']) {
      await expect(verifyEnvelope(await f.sign({ ...f.payload, notes }), f.publicKey)).resolves.toMatchObject({ notes });
    }
    for (const notes of [null, 42, {}, 'x'.repeat(513), '中'.repeat(171)]) expect(() => validatePayload({ ...f.payload, notes })).toThrow('512');
    expect(() => validatePayload({ ...f.payload, notes: 'ok', extra: true })).toThrow('字段');
  });
  it('rejects an oversized signed envelope even if decoded payload is below 4096 bytes', async () => {
    const f = await fixture();
    const envelope = await f.sign({ ...f.payload, notes: 'x'.repeat(2000) });
    expect(atob(envelope.payload).length).toBeLessThan(MAX_ENVELOPE_BYTES);
    expect(new TextEncoder().encode(JSON.stringify(envelope)).length).toBeGreaterThan(MAX_ENVELOPE_BYTES);
    await expect(verifyEnvelope(envelope, f.publicKey)).rejects.toThrow('4096');
  });
  it('caps the raw network envelope at exactly 4096 bytes including whitespace', async () => {
    const f = await fixture(), url = 'https://qmqsu.github.io/folio/channels/preview-usb.json';
    const json = JSON.stringify(f.envelope);
    const response = (size: number, declared = false) => {
      const r = new Response(json.padEnd(size, ' '), { headers: declared ? { 'content-length': String(size) } : {} });
      Object.defineProperty(r, 'url', { value: url }); return r;
    };
    vi.stubGlobal('fetch', async () => response(4096));
    await expect(fetchRelease('preview')).rejects.toThrow('签名'); // Within size bound, ephemeral key is not trusted in production.
    vi.stubGlobal('fetch', async () => response(4097));
    await expect(fetchRelease('preview')).rejects.toThrow('超过签名长度');
    vi.stubGlobal('fetch', async () => response(4097, true));
    await expect(fetchRelease('preview')).rejects.toThrow('下载长度超出限制');
  });
  it('uses the exact CDN mirror only after a primary transport failure', async () => {
    const f = await fixture();
    const envelope = await f.sign({ ...f.payload, channel: 'stable' });
    const primary = 'https://qmqsu.github.io/folio/channels/stable-usb.json';
    const mirror = mirrorUrl(primary)!;
    const calls: string[] = [];
    vi.stubGlobal('fetch', async (url: string) => {
      calls.push(url);
      if (url === primary) throw new TypeError('fetch failed');
      const response = new Response(JSON.stringify(envelope));
      Object.defineProperty(response, 'url', { value: url });
      return response;
    });
    const bytes = await fetchWithMirror(primary, 4096);
    expect(new TextDecoder().decode(bytes)).toBe(JSON.stringify(envelope));
    expect(calls).toEqual([primary, mirror]);
    expect(mirror).toContain('testingcf.jsdelivr.net/gh/qmqsu/folio@folio-release-assets/');
  });
  it('tries the mirror on a stalled connection and bounds both attempts', async () => {
    vi.useFakeTimers();
    try {
      const fetch = vi.fn((_url, { signal }) => new Promise((_resolve, reject) => {
        signal.addEventListener('abort', () => reject(signal.reason));
      }));
      vi.stubGlobal('fetch', fetch);
      const url = 'https://qmqsu.github.io/folio/channels/stable-usb.json';
      const result = expect(fetchWithMirror(url, 4096)).rejects.toThrow('下载超时');
      await vi.advanceTimersByTimeAsync(60000);
      await result;
      expect(fetch.mock.calls.map(call => call[0])).toEqual([url, mirrorUrl(url)]);
    } finally { vi.useRealTimers(); }
  });
  it('does not mirror an unpublished 404 or an arbitrary origin', async () => {
    const primary = 'https://qmqsu.github.io/folio/channels/stable-usb.json';
    const calls: string[] = [];
    vi.stubGlobal('fetch', async (url: string) => {
      calls.push(url);
      const response = new Response(null, { status: 404 });
      Object.defineProperty(response, 'url', { value: url });
      return response;
    });
    await expect(fetchWithMirror(primary, 4096)).rejects.toThrow('未发布');
    expect(calls).toEqual([primary]);
    expect(mirrorUrl('https://example.com/firmware/folio.bin')).toBeUndefined();
  });
  it('rejects malformed/noncanonical envelope encoding and extra envelope keys', async () => {
    const f = await fixture();
    for (const envelope of [{ ...f.envelope, payload: '!!!!' }, { ...f.envelope, signature: 'AAAA' }, { ...f.envelope, public_key: f.publicKey }]) await expect(verifyEnvelope(envelope, f.publicKey)).rejects.toThrow();
  });
});
describe('actual partition bytes including complete 4096-byte physical sector', () => {
  it('supports exact Folio layout and the specifically sourced official layout', () => {
    expect(assessInstalledLayout(table())).toMatchObject({ compatible: true, migration: false });
    expect(assessInstalledLayout(table(true))).toMatchObject({ compatible: true, migration: true });
    expect(() => validatePartitionTable(table(true))).toThrow(); // Target must still be Folio v2.
  });
  it('rejects non-erased sector padding, bad MD5, encrypted flags and truncated tables', () => {
    for (const index of [20, 28, 240, 3072, 4095]) {
      const bytes = table(); bytes[index] ^= 1; expect(assessInstalledLayout(bytes).compatible).toBe(false);
    }
    expect(assessInstalledLayout(table().slice(0, 3072)).compatible).toBe(false);
    expect(assessInstalledLayout(new Uint8Array(4096)).compatible).toBe(false);
  });
});
