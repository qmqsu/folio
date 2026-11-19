import { md5 } from '@noble/hashes/legacy.js';

export const EXPECTED_PARTITIONS: Readonly<Record<string, readonly number[]>> = Object.freeze({
  nvs: [1, 2, 0x9000, 0x5000], phy_init: [1, 1, 0xe000, 0x1000],
  factory: [0, 0, 0x10000, 0x400000], storage: [1, 0x81, 0x410000, 0x500000],
  otadata: [1, 0, 0x910000, 0x2000], ota_0: [0, 0x10, 0x920000, 0x300000], ota_1: [0, 0x11, 0xc20000, 0x300000],
});
export const OFFICIAL_LAYOUT_SOURCE = 'https://github.com/MindReset/read_pico_firmware/blob/6a247cc94420b85c650b32a6b2c063a360070eee/partitions_16M.csv';
export const OFFICIAL_PARTITIONS = Object.freeze(Object.fromEntries(Object.entries(EXPECTED_PARTITIONS).slice(0, 4)));
export function validatePartitionTable(data: Uint8Array, expectedLayout = EXPECTED_PARTITIONS): void {
  if (!data.length || data.length > 4096 || data.length % 32) throw new Error('分区表长度无效。');
  const entries = new Map<string, number[]>();
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let hasDigest = false;
  for (let at = 0; at < data.length; at += 32) {
    const entry = data.subarray(at, at + 32);
    if (entry.every(b => b === 255)) {
      if (!data.subarray(at).every(b => b === 255)) throw new Error('分区表末尾不是完整擦除态。');
      break;
    }
    if (view.getUint16(at, true) === 0xebeb) {
      if (hasDigest || !entry.subarray(2, 16).every(b => b === 255) || !md5(data.subarray(0, at)).every((b, i) => b === entry[16 + i])) throw new Error('分区表 MD5 不符。');
      hasDigest = true; continue;
    }
    if (hasDigest || view.getUint16(at, true) !== 0x50aa || view.getUint32(at + 28, true) !== 0) throw new Error('分区表记录无效或加密。');
    const nameBytes = entry.subarray(12, 28), zero = nameBytes.indexOf(0);
    if (zero <= 0 || nameBytes.subarray(zero).some(b => b !== 0) || nameBytes.subarray(0, zero).some(b => b > 127)) throw new Error('分区名无效。');
    const name = new TextDecoder().decode(nameBytes.subarray(0, zero));
    if (entries.has(name)) throw new Error('分区重复。');
    entries.set(name, [entry[2], entry[3], view.getUint32(at + 4, true), view.getUint32(at + 8, true)]);
  }
  if (!hasDigest || entries.size !== Object.keys(expectedLayout).length) throw new Error('不是已支持的分区布局。');
  for (const [name, expected] of Object.entries(expectedLayout)) {
    const actual = entries.get(name);
    if (!actual || actual.some((v, i) => v !== expected[i])) throw new Error(`分区 ${name} 不符合已支持布局。`);
  }
}
// https://github.com/MindReset/read_pico_firmware/blob/28e7e13669e04764e10b97b190e769ba578e2270/partitions_16M.csv
export const SPIFFS_PARTITIONS = Object.freeze({
  nvs: [1, 2, 0x9000, 0x5000], phy_init: [1, 1, 0xe000, 0x1000],
  factory: [0, 0, 0x10000, 0x200000], spiffs: [1, 0x82, 0x210000, 0x500000],
});
export type LayoutAssessment = { compatible: boolean; label: string } & (
  { migration: false } | { migration: true; blankFrom: 0x210000 | 0x910000 }
);
export function assessInstalledLayout(table: Uint8Array): LayoutAssessment {
  if (table.length === 4096) {
    const candidates = [
      [EXPECTED_PARTITIONS, { compatible: true, label: 'Folio 16MiB factory/storage/OTA v2', migration: false }],
      [OFFICIAL_PARTITIONS, { compatible: true, label: '官方 6a247cc：4 MiB factory + FAT 四分区（需独立授权迁移）', migration: true, blankFrom: 0x910000 }],
      [SPIFFS_PARTITIONS, { compatible: true, label: '官方 28e7e13：2 MiB factory + SPIFFS 四分区（原 SPIFFS 与新版 factory/storage 重叠，需独立授权迁移）', migration: true, blankFrom: 0x210000 }],
    ] as const;
    for (const [entries, assessment] of candidates) {
      try { validatePartitionTable(table, entries); return assessment; } catch { /* not an exact match */ }
    }
  }
  return { compatible: false, label: '未知或暂不支持的原厂/第三方布局：仅备份，禁止网页写入', migration: false };
}
