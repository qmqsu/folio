import { md5 } from '@noble/hashes/legacy.js';
import { EXPECTED_PARTITIONS, OFFICIAL_PARTITIONS, THINGSOS_PARTITIONS } from '../src/installer/partition';
import { FLASH_SIZE, KEY_ID, LAYOUT, hex, sha256, verifyEnvelope, type Release } from '../src/installer/manifest';
import { SUPPORTED_FLASH_ID, type SerialDevice, type DeviceInfo } from '../src/installer/controller';

export function table(official: boolean | 'spiffs' | 'thingsos' = false, length = 4096): Uint8Array {
  // Independent fixture from upstream's initial partition CSV (28e7e13).
  const entries = Object.entries(official === 'thingsos' ? THINGSOS_PARTITIONS : official === 'spiffs' ? { nvs: [1, 2, 0x9000, 0x5000], phy_init: [1, 1, 0xe000, 0x1000], factory: [0, 0, 0x10000, 0x200000], spiffs: [1, 0x82, 0x210000, 0x500000] } : official ? OFFICIAL_PARTITIONS : EXPECTED_PARTITIONS);
  const bytes = new Uint8Array(length).fill(255), view = new DataView(bytes.buffer);
  for (let i = 0; i < entries.length; i++) {
    const [name, [type, subtype, offset, size]] = entries[i], at = i * 32;
    view.setUint16(at, 0x50aa, true); bytes[at + 2] = type; bytes[at + 3] = subtype;
    view.setUint32(at + 4, offset, true); view.setUint32(at + 8, size, true);
    bytes.fill(0, at + 12, at + 32); bytes.set(new TextEncoder().encode(name), at + 12);
  }
  const at = entries.length * 32;
  view.setUint16(at, 0xebeb, true); bytes.set(md5(bytes.subarray(0, at)), at + 16);
  return bytes;
}
export async function fixture() {
  const boot = new Uint8Array(32), app = new Uint8Array(256), ota = new Uint8Array(8192).fill(255);
  for (const bytes of [boot, app]) { bytes[0] = 0xe9; bytes[12] = 9; bytes[2] = 2; bytes[3] = 0x4f; }
  new DataView(app.buffer).setUint32(32, 0xabcd5432, true);
  app.set(new TextEncoder().encode('0.8.32-preview'), 48); app.set(new TextEncoder().encode('Folio'), 80);
  const images = [boot, table(false, 3072), app, ota];
  const payload = { schema: 1, kind: 'usb', project: 'Folio', chip: 'esp32s3', hardware: 'RDP-G01-W', layout: LAYOUT,
    channel: 'preview', version: '0.8.32-preview', sequence: 832, flash_size: FLASH_SIZE,
    parts: await Promise.all(images.map(async (bytes, i) => ({ url: `https://qmqsu.github.io/folio/firmware/0.8.32-preview/${['bootloader', 'partition-table', 'folio', 'ota_data_initial'][i]}.bin`, offset: [0, 0x8000, 0x10000, 0x910000][i], size: bytes.length, sha256: await sha256(bytes) }))) };
  const pair = await crypto.subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
  const publicKey = new Uint8Array(await crypto.subtle.exportKey('raw', pair.publicKey));
  const sign = async (value: unknown) => {
    const bytes = new TextEncoder().encode(JSON.stringify(value));
    const signature = new Uint8Array(await crypto.subtle.sign({ name: 'ECDSA', hash: 'SHA-256' }, pair.privateKey, bytes));
    const b64 = (v: Uint8Array) => btoa(String.fromCharCode(...v));
    return { key_id: KEY_ID, payload: b64(bytes), signature: b64(signature) };
  };
  const envelope = await sign(payload);
  const release = await verifyEnvelope(envelope, publicKey);
  const download = async (url: string, _max: number, signal?: AbortSignal, progress?: (bytes: number) => void) => {
    signal?.throwIfAborted();
    const index = payload.parts.findIndex(p => p.url === url);
    const bytes = images[index].slice(); progress?.(bytes.length); return bytes;
  };
  return { payload, images, publicKey, sign, envelope, release, download };
}
export class FakeDevice implements SerialDevice {
  memory: Uint8Array;
  info: DeviceInfo = { chip: 'ESP32-S3', flashSize: FLASH_SIZE, flashId: SUPPORTED_FLASH_ID, securitySafe: true };
  writes: number[] = []; closed = 0; connected = 0; resets = 0; shortBackup = false; corruptReadback = false;
  writeFailure = false; closeFailure = false; onRead?: (offset: number, size: number) => void; onMd5?: (offset: number, size: number) => void;
  constructor(official: boolean | 'spiffs' | 'thingsos' = false) { this.memory = new Uint8Array(FLASH_SIZE).fill(0xa5); this.memory.set(table(official), 0x8000); }
  async connect(signal: AbortSignal): Promise<DeviceInfo> { signal.throwIfAborted(); this.connected++; return this.info; }
  async read(offset: number, size: number, signal: AbortSignal, progress: (bytes: number) => void): Promise<Uint8Array> {
    this.onRead?.(offset, size); signal.throwIfAborted(); progress(size);
    const output = this.memory.slice(offset, offset + size - (this.shortBackup && size === FLASH_SIZE ? 1 : 0));
    if (this.corruptReadback && this.writes.includes(offset)) output[0] ^= 1;
    return output;
  }
  async md5(offset: number, size: number, signal: AbortSignal): Promise<string> {
    this.onMd5?.(offset, size); signal.throwIfAborted();
    const bytes = this.memory.slice(offset, offset + size);
    if (this.corruptReadback && this.writes.includes(offset)) bytes[0] ^= 1;
    return hex(md5(bytes));
  }
  async reset(signal: AbortSignal): Promise<void> { this.resets++; signal.throwIfAborted(); }
  async write(part: Release['parts'][number], bytes: Uint8Array, signal: AbortSignal, progress: (bytes: number, total: number) => void): Promise<void> {
    signal.throwIfAborted(); this.writes.push(part.offset);
    if (this.writeFailure) throw new Error('Simulated write failure');
    this.memory.set(bytes, part.offset); progress(bytes.length, bytes.length);
  }
  async close(): Promise<void> { this.closed++; if (this.closeFailure) throw new Error('Simulated close failure'); }
}
