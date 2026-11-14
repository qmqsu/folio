import { validatePartitionTable } from './partition';

export const ORIGIN = 'https://qmqsu.github.io';
export const FLASH_SIZE = 0x1000000;
export const MAX_ENVELOPE_BYTES = 4096;
export const LAYOUT = 'folio-16m-factory-storage-ota-v2';
export const KEY_ID = 'folio-release-2026-2';
// Public trust anchor supplied by the release owner. Never fetch a key from a manifest.
export const PUBLIC_KEY_HEX = '0467c1efb911dea6df8e1818491b54c2fc791acda21a968b6d25aa99833a46b58c52b1e3ba1dbc90263d0d3c0166b3b65416742f5af4fb1daa93e32bd199d774df';
export const PUBLIC_KEY_SHA256 = '2e3d9f0853c515c0e650224ba68cd169baf50534ce638ea54b86826e8b763e6a';
export interface Part { readonly url: string; readonly offset: number; readonly size: number; readonly sha256: string }
export interface Release {
  readonly schema: 1; readonly kind: 'usb'; readonly project: 'Folio'; readonly chip: 'esp32s3';
  readonly hardware: 'RDP-G01-W'; readonly layout: typeof LAYOUT;
  readonly channel: 'preview' | 'stable'; readonly version: string; readonly sequence: number;
  readonly flash_size: typeof FLASH_SIZE; readonly parts: readonly Part[]; readonly notes?: string;
}
const verified = new WeakSet<object>();
export function assertVerified(release: Release): void {
  if (!verified.has(release)) throw new Error('发布清单尚未通过固定公钥签名验证。');
}
export function hex(bytes: Uint8Array): string { return Array.from(bytes, b => b.toString(16).padStart(2, '0')).join(''); }
export async function sha256(bytes: Uint8Array): Promise<string> {
  return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', new Uint8Array(bytes))));
}
export function exactKeys(input: unknown, keys: string[]): asserts input is Record<string, unknown> {
  if (!input || typeof input !== 'object' || Array.isArray(input) || Object.keys(input).sort().join(',') !== [...keys].sort().join(',')) {
    throw new Error('清单字段不完整或含不受支持的字段。');
  }
}
function base64(value: unknown, maximum: number): Uint8Array {
  if (typeof value !== 'string' || value.length > maximum * 1.4 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) throw new Error('签名编码无效。');
  const decoded = atob(value);
  if (decoded.length > maximum || btoa(decoded) !== value) throw new Error('签名编码不规范。');
  return Uint8Array.from(decoded, c => c.charCodeAt(0));
}
export function firmwareUrl(value: unknown): string {
  if (typeof value !== 'string') throw new Error('镜像地址缺失。');
  const url = new URL(value);
  if (url.origin !== ORIGIN || url.protocol !== 'https:' || url.username || url.password || url.search || url.hash ||
    !/^\/folio\/firmware\/[A-Za-z0-9_-][A-Za-z0-9._-]*\/[A-Za-z0-9_-][A-Za-z0-9._-]*\.bin$/.test(url.pathname) ||
    value !== url.href || value.includes('..') || value.includes('%')) throw new Error('镜像必须来自固定 HTTPS 发布目录。');
  return value;
}
export function validatePayload(input: unknown): Release {
  const hasNotes = !!input && typeof input === 'object' && Object.hasOwn(input, 'notes');
  exactKeys(input, ['schema', 'kind', 'project', 'chip', 'hardware', 'layout', 'channel', 'version', 'sequence', 'flash_size', 'parts', ...(hasNotes ? ['notes'] : [])]);
  if (hasNotes && (typeof input.notes !== 'string' || new TextEncoder().encode(input.notes).length > 512)) throw new Error('发布说明须为最多 512 UTF-8 字节的文本。');
  if (input.schema !== 1 || input.kind !== 'usb' || input.project !== 'Folio' || input.chip !== 'esp32s3' || input.hardware !== 'RDP-G01-W' || input.layout !== LAYOUT || input.flash_size !== FLASH_SIZE) throw new Error('不是适用于本硬件与布局的 USB 发布清单。');
  if (!['preview', 'stable'].includes(input.channel as string) || typeof input.version !== 'string' || !/^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$/.test(input.version) || input.version.length > 31 || !Number.isInteger(input.sequence) || (input.sequence as number) < 0 || (input.sequence as number) > 0xffffffff) throw new Error('发布通道、版本或序列号无效。');
  if (!Array.isArray(input.parts) || input.parts.length !== 4) throw new Error('USB 必须恰好包含四段镜像。');
  const limits = new Map([[0, 0x8000], [0x8000, 0x1000], [0x10000, 0x300000], [0x910000, 0x2000]]);
  const filenames = new Map([[0, 'bootloader.bin'], [0x8000, 'partition-table.bin'], [0x10000, 'folio.bin'], [0x910000, 'ota_data_initial.bin']]);
  const offsets = new Set<number>();
  const urls = new Set<string>();
  const parts = input.parts.map(part => {
    exactKeys(part, ['url', 'offset', 'size', 'sha256']);
    const offset = part.offset as number, size = part.size as number;
    if (!Number.isInteger(offset) || !limits.has(offset) || offsets.has(offset) || !Number.isInteger(size) || size <= 0 || size > limits.get(offset)!) throw new Error('镜像地址或长度越过四段白名单。');
    if ((offset === 0x8000 && size % 32 !== 0) || (offset === 0x910000 && size !== 8192)) throw new Error('分区表或 OTA 初始化长度错误。');
    if (typeof part.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(part.sha256)) throw new Error('SHA-256 格式错误。');
    const url = firmwareUrl(part.url);
    if (url !== `${ORIGIN}/folio/firmware/${input.version}/${filenames.get(offset)}`) throw new Error('镜像 URL 必须精确匹配签名版本与该地址的固定文件名。');
    if (urls.has(url)) throw new Error('镜像 URL 重复。');
    offsets.add(offset); urls.add(url);
    return Object.freeze({ url, offset, size, sha256: part.sha256 });
  }).sort((a, b) => a.offset - b.offset);
  for (let i = 1; i < parts.length; i++) {
    if (parts[i - 1].offset + Math.ceil(parts[i - 1].size / 4096) * 4096 > parts[i].offset) throw new Error('镜像擦除扇区重叠。');
  }
  return Object.freeze({ ...input, parts: Object.freeze(parts) }) as unknown as Release;
}
// The optional raw key is a dependency-injection seam for cryptographic tests only;
// production callers never supply it. Neither envelope nor catalog can supply a key.
export async function verifyEnvelope(envelope: unknown, rawKey = Uint8Array.from(PUBLIC_KEY_HEX.match(/../g)!, s => parseInt(s, 16))): Promise<Release> {
  exactKeys(envelope, ['key_id', 'payload', 'signature']);
  if (envelope.key_id !== KEY_ID) throw new Error('未知发布签名密钥。');
  if (new TextEncoder().encode(JSON.stringify(envelope)).length > MAX_ENVELOPE_BYTES) throw new Error('签名信封超过 4096 字节。');
  const payload = base64(envelope.payload, MAX_ENVELOPE_BYTES), signature = base64(envelope.signature, 64);
  if (signature.length !== 64 || !payload.length || payload[0] === 0xef) throw new Error('签名或 UTF-8 载荷无效。');
  const key = await crypto.subtle.importKey('raw', new Uint8Array(rawKey), { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
  if (!await crypto.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key, new Uint8Array(signature), new Uint8Array(payload))) throw new Error('发布者签名校验失败，已阻止安装。');
  const release = validatePayload(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(payload)));
  verified.add(release);
  return release;
}
export async function fetchBounded(url: string, maximum: number, signal?: AbortSignal, progress?: (bytes: number) => void): Promise<Uint8Array> {
  const response = await fetch(url, { signal, redirect: 'error', credentials: 'omit', cache: 'no-store', referrerPolicy: 'no-referrer' });
  if (!response.ok) throw new Error(response.status === 404 ? '未发布：此通道尚无可用清单。' : `下载失败（HTTP ${response.status}）。`);
  if (response.url !== url) throw new Error('下载地址发生变化。');
  const declared = response.headers.get('content-length');
  if (declared && Number(declared) > maximum) throw new Error('下载长度超出限制。');
  if (!response.body) throw new Error('下载流不可用。');
  const reader = response.body.getReader();
  const output = new Uint8Array(maximum);
  let length = 0;
  try {
    for (;;) {
      signal?.throwIfAborted();
      const { done, value } = await reader.read();
      if (done) break;
      if (length + value.length > maximum) throw new Error('下载内容超过签名长度。');
      output.set(value, length); length += value.length; progress?.(length);
    }
  } finally { await reader.cancel().catch(() => {}); reader.releaseLock(); }
  return output.slice(0, length);
}
export async function fetchRelease(channel: 'preview' | 'stable', signal?: AbortSignal): Promise<Release> {
  const url = `${ORIGIN}/folio/channels/${channel}-usb.json`;
  const bytes = await fetchBounded(url, MAX_ENVELOPE_BYTES, signal);
  const release = await verifyEnvelope(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes)));
  if (release.channel !== channel) throw new Error('签名通道与所选通道不符。');
  return release;
}
export async function validateImage(part: Part, data: Uint8Array, release: Release): Promise<void> {
  if (data.length !== part.size || await sha256(data) !== part.sha256) throw new Error(`0x${part.offset.toString(16)} 镜像长度或 SHA-256 不符。`);
  if (part.offset === 0x8000) validatePartitionTable(data);
  if (part.offset === 0x910000 && !data.every(b => b === 0xff)) throw new Error('OTA 初始化必须为 8192 字节的擦除态。');
  if (part.offset === 0 || part.offset === 0x10000) {
    if (data.length < 24 || data[0] !== 0xe9 || data[12] !== 9 || data[13] !== 0) throw new Error('不是 ESP32-S3 应用/启动镜像。');
    if (part.offset === 0 && (data[2] !== 2 || data[3] !== 0x4f)) throw new Error('启动镜像须声明 DIO / 16MB / 80MHz。');
    if (part.offset === 0x10000) {
      const field = (start: number) => new TextDecoder().decode(data.slice(start, start + 32)).split('\0')[0];
      if (data.length < 112 || new DataView(data.buffer, data.byteOffset).getUint32(32, true) !== 0xabcd5432 || field(48) !== release.version || field(80) !== 'Folio') throw new Error('应用内项目名/版本与签名不符。');
    }
  }
}
