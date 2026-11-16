import { describe, it, expect, vi } from 'vitest';
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

  it('writes four segments and verifies them plus protected regions on device', async () => {
    const { flash, device, updates } = await setup();
    const seen: number[] = [];
    device.onMd5 = offset => seen.push(offset);
    const nvs = device.memory.slice(0x9000, 0x10000);
    const storage = device.memory.slice(0x410000, 0x910000);
    await flash.run();
    expect(flash.stage).toBe('success');
    expect(device.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);
    expect(seen.slice(0, 2)).toEqual([0x9000, 0x410000]);
    expect(seen.filter(offset => offset === 0x9000)).toHaveLength(2);
    expect(seen.filter(offset => offset === 0x410000)).toHaveLength(2);
    for (const offset of [0, 0x8000, 0x10000, 0x910000]) expect(seen).toContain(offset);
    expect(nvs.every((byte, i) => byte === device.memory[0x9000 + i])).toBe(true);
    expect(storage.every((byte, i) => byte === device.memory[0x410000 + i])).toBe(true);
    expect(device.resets).toBe(1);
    expect(device.closed).toBe(1);
    expect(flash.connected).toBe(false);
    expect(updates.filter(u => u.stage === 'verifying').length).toBeGreaterThanOrEqual(8);
    expect(updates.at(-1)?.message).toContain('已重启');
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

    const factory = await setup(true);
    await factory.flash.run();
    expect(factory.flash.stage).toBe('failed');
    expect(factory.device.writes).toEqual([]);
  });

  it('stops when the device MD5 does not match the signed image', async () => {
    const { flash, device } = await setup();
    device.corruptReadback = true;
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(device.closed).toBe(1);
  });

  it('only downloads the 4 KiB partition table, never a full backup', async () => {
    const { flash, device } = await setup();
    const reads: number[] = [];
    device.onRead = (_offset, size) => reads.push(size);
    await flash.run();
    expect(flash.stage).toBe('success');
    expect(reads).toEqual([4096]);
    expect(reads).not.toContain(FLASH_SIZE);
    expect(device.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);
  });

  it('detects a protected-region MD5 change', async () => {
    const { flash, device } = await setup();
    let fontCalls = 0;
    device.onMd5 = offset => {
      if (offset === 0x410000 && ++fontCalls === 2) device.memory[0x410000] ^= 1;
    };
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(device.closed).toBe(1);
    expect(flash.connected).toBe(false);
  });

  it('reports a connection failure even when the browser close never settles', async () => {
    const { flash, device, updates } = await setup();
    vi.spyOn(device, 'connect').mockRejectedValue(new Error('连接设备超时'));
    vi.spyOn(device, 'close').mockImplementation(() => new Promise<void>(() => {}));
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(device.writes).toEqual([]);
    expect(updates.at(-1)?.message).toContain('连接设备超时');
    expect(updates.at(-1)?.message).toContain('串口没关掉');
  }, 10000);

  it('cancels, closes the port and never reports success', async () => {
    const { flash, device } = await setup();
    device.onRead = () => { void flash.cancel(); };
    await flash.run();
    expect(flash.stage).toBe('cancelled');
    expect(device.closed).toBeGreaterThanOrEqual(1);
    expect(flash.connected).toBe(false);
  });
});
