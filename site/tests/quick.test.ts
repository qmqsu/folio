import { describe, it, expect } from 'vitest';
import { QuickFlash, type QuickUpdate } from '../src/installer/quick';
import { FLASH_SIZE } from '../src/installer/manifest';
import { FakeDevice, fixture } from './fixtures';

async function setup(official = false) {
  const f = await fixture();
  const device = new FakeDevice(official);
  const updates: QuickUpdate[] = [];
  const flash = new QuickFlash(f.release, device, update => updates.push(update), f.download);
  return { ...f, device, updates, flash };
}

describe('one-click flash (simulated device only)', () => {
  it('refuses a release object that never passed signature verification', async () => {
    const { release, device, download } = await setup();
    expect(() => new QuickFlash({ ...release }, device, () => {}, download)).toThrow('签名');
  });

  it('verifies every image before it touches the port', async () => {
    const { release, device } = await setup();
    const flash = new QuickFlash(release, device, () => {}, async () => new Uint8Array(32));
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(device.connected).toBe(0);
    expect(device.writes).toEqual([]);
  });

  it('writes only the four whitelisted segments and reads each one back', async () => {
    const { flash, device, updates } = await setup();
    const nvs = device.memory.slice(0x9000, 0x10000);
    const storage = device.memory.slice(0x410000, 0x910000);
    await flash.run();
    expect(flash.stage).toBe('success');
    expect(device.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);
    expect(nvs.every((byte, i) => byte === device.memory[0x9000 + i])).toBe(true);
    expect(storage.every((byte, i) => byte === device.memory[0x410000 + i])).toBe(true);
    expect(device.closed).toBe(1);
    expect(flash.connected).toBe(false);
    expect(updates.filter(u => u.stage === 'verifying').length).toBeGreaterThanOrEqual(4);
    expect(updates.at(-1)?.message).toContain('重启');
  });

  it('never flashes a device it cannot identify', async () => {
    for (const key of ['chip', 'flashSize', 'securitySafe'] as const) {
      const { flash, device } = await setup();
      (device.info as unknown as Record<string, unknown>)[key] = key === 'chip' ? 'ESP32-C3' : key === 'flashSize' ? FLASH_SIZE / 2 : false;
      await flash.run();
      expect(flash.stage).toBe('failed');
      expect(device.writes).toEqual([]);
      expect(device.closed).toBe(1);
    }
  });

  it('refuses unvalidated flash vendors and unknown or factory layouts', async () => {
    const vendor = await setup();
    vendor.device.info.flashId = 0x1840ef;
    await vendor.flash.run();
    expect(vendor.flash.stage).toBe('failed');
    expect(vendor.device.writes).toEqual([]);

    const unknown = await setup();
    unknown.device.memory.fill(0xff, 0x8000, 0x9000);
    await unknown.flash.run();
    expect(unknown.flash.stage).toBe('failed');
    expect(unknown.device.writes).toEqual([]);

    // A factory four-partition layout is compatible for the full installer only after
    // an explicit migration authorization, so one-click must refuse it.
    const factory = await setup(true);
    await factory.flash.run();
    expect(factory.flash.stage).toBe('failed');
    expect(factory.device.writes).toEqual([]);
  });

  it('stops when the readback does not match the signed image', async () => {
    const { flash, device } = await setup();
    device.corruptReadback = true;
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(device.closed).toBe(1);
  });

  it('always takes a fresh full backup and rejects backup opt-out', async () => {
    const reads: number[] = [];
    const seen: { size: number; sha: string }[] = [];
    const withBackup = await setup();
    withBackup.device.onRead = (_offset, size) => reads.push(size);
    await withBackup.flash.run({ onBackup: ({ bytes, sha256 }) => seen.push({ size: bytes.length, sha: sha256 }) });
    expect(withBackup.flash.stage).toBe('success');
    expect(reads).toContain(FLASH_SIZE);
    expect(seen).toHaveLength(1);
    expect(seen[0].size).toBe(FLASH_SIZE);
    expect(seen[0].sha).toMatch(/^[a-f0-9]{64}$/);
    expect(withBackup.updates.some(u => u.stage === 'backing-up')).toBe(true);
    expect(withBackup.device.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);

    const refused = await setup();
    await refused.flash.run({ backup: false });
    expect(refused.flash.stage).toBe('failed');
    expect(refused.device.connected).toBe(0);
    expect(refused.device.writes).toEqual([]);
  });

  it('verifies NVS/PHY and the 5 MiB font resource area against the fresh backup', async () => {
    const { flash, device } = await setup();
    device.onRead = offset => {
      if (offset === 0x410000) device.memory[0x410000] ^= 1;
    };
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(device.closed).toBe(1);
    expect(flash.connected).toBe(false);
  });

  it('stops before writing when the backup read is incomplete', async () => {
    const { flash, device } = await setup();
    device.shortBackup = true;
    await flash.run({ backup: true });
    expect(flash.stage).toBe('failed');
    expect(device.writes).toEqual([]);
    expect(device.closed).toBe(1);
  });

  it('cancels, closes the port and never reports success', async () => {
    const { flash, device } = await setup();
    device.onRead = () => { void flash.cancel(); };
    await flash.run();
    expect(flash.stage).toBe('cancelled');
    expect(device.closed).toBeGreaterThanOrEqual(1);
    expect(flash.connected).toBe(false);
  });
});
