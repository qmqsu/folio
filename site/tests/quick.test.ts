import { describe, it, expect, vi } from 'vitest';
import { QuickFlash, type QuickUpdate } from '../src/installer/quick';
import { FLASH_SIZE } from '../src/installer/manifest';
import { FakeDevice, fixture } from './fixtures';

async function setup(official: boolean | 'spiffs' = false) {
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
    expect(seen.slice(0, 3)).toEqual([0x8000, 0x9000, 0x410000]);
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

    for (const kind of [true, 'spiffs'] as const) {
      const factory = await setup(kind); await factory.flash.run();
      expect(factory.flash.stage).toBe('failed'); expect(factory.device.writes).toEqual([]);
    }
  });

  it('backs up a foreign layout before replacing it, and honours cancellation and later changes', async () => {
    for (const outcome of ['install', 'cancel', 'changed']) {
      const { flash, device } = await setup('spiffs');
      await flash.run(async backup => {
        expect(backup.length).toBe(FLASH_SIZE);
        expect(backup.every((byte, i) => byte === device.memory[i])).toBe(true);
        expect(device.writes).toEqual([]);
        if (outcome === 'changed') device.memory[0x9000] ^= 1;
        return outcome !== 'cancel';
      });
      expect(flash.stage).toBe(outcome === 'install' ? 'success' : 'failed');
      expect(device.writes.length).toBe(outcome === 'install' ? 4 : 0);
    }
  });

  it('reconnects once before writing when the initial partition read fails', async () => {
    const { flash, device } = await setup();
    device.memory[0x8fff] = 0;
    vi.spyOn(device, 'read').mockRejectedValueOnce(new Error('serial read failed'));
    const second = new FakeDevice(false);
    await flash.run(undefined, () => second);
    expect(device.writes).toEqual([]);
    expect(device.closed).toBe(1);
    expect(second.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);
    expect(flash.stage).toBe('success');
  });

  it('stops when the device MD5 does not match the signed image', async () => {
    const { flash, device } = await setup();
    device.corruptReadback = true;
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(device.closed).toBe(1);
  });

  it('reads the whole sector and stops if its padding is not erased', async () => {
    const { flash, device } = await setup();
    device.memory[0x8fff] = 0;
    const reads: number[] = [];
    device.onRead = (_offset, size) => reads.push(size);
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(reads).toEqual([4096]);
    expect(reads).not.toContain(FLASH_SIZE);
    expect(device.writes).toEqual([]);
  });

  it('recognises a byte-identical signed partition sector without streaming it', async () => {
    const { flash, device, release, download } = await setup();
    const part = release.parts.find(part => part.offset === 0x8000)!;
    device.memory.fill(0xff, 0x8000, 0x9000);
    device.memory.set(await download(part.url, part.size), 0x8000);
    const read = vi.spyOn(device, 'read');
    await flash.run();
    expect(read).not.toHaveBeenCalled();
    expect(flash.stage).toBe('success');
  });

  it('identifies a partition timeout after successful chip detection', async () => {
    const { flash, device, updates } = await setup();
    device.memory[0x8fff] = 0;
    vi.spyOn(device, 'read').mockRejectedValue(new Error('No serial data received'));
    await flash.run();
    expect(updates.at(-1)?.message).toContain('检查分区表');
    expect(device.writes).toEqual([]);
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

  it('checks PHY data as well as the NVS partition', async () => {
    const { flash, device } = await setup();
    device.onMd5 = offset => { if (device.writes.length && offset === 0x9000) device.memory[0xe000] ^= 1; };
    await flash.run();
    expect(flash.stage).toBe('failed'); expect(device.resets).toBe(0);
  });

  it('turns no serial data into actionable port instructions', async () => {
    const { flash, device, updates } = await setup();
    vi.spyOn(device, 'connect').mockRejectedValue(new Error('No serial data received'));
    await flash.run();
    expect(flash.stage).toBe('failed');
    expect(updates.at(-1)?.message).toContain('USB JTAG_serial debug unit');
    expect(updates.at(-1)?.message).toContain('短按电源键');
    expect(device.writes).toEqual([]);
  });

  it('reports verified installation when closing after reset stalls', async () => {
    const { flash, device, updates } = await setup();
    vi.spyOn(device, 'close').mockImplementation(() => new Promise<void>(() => {}));
    await flash.run();
    expect(device.resets).toBe(1);
    expect(flash.stage).toBe('success');
    expect(updates.at(-1)?.message).toContain('关闭此页');
  }, 10000);

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
    device.onMd5 = () => { void flash.cancel(); };
    await flash.run();
    expect(flash.stage).toBe('cancelled');
    expect(device.closed).toBeGreaterThanOrEqual(1);
    expect(flash.connected).toBe(false);
  });
});
