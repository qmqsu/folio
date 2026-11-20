import { describe, it, expect, vi } from 'vitest';
import { Installer, type Update } from '../src/installer/controller';
import { FLASH_SIZE } from '../src/installer/manifest';
import { FakeDevice, fixture } from './fixtures';

async function setup(official: boolean | 'spiffs' | 'thingsos' = false) {
  const f = await fixture(), device = new FakeDevice(official), updates: Update[] = [];
  const installer = new Installer(f.release, device, update => updates.push(update), f.download);
  const save = () => installer.confirmBackupSaved(installer.evidence!.sha256, true, true, true);
  return { ...f, device, updates, installer, save };
}
describe('destructive-operation safety state machine (simulated device only)', () => {
  it('never connects without physical RDP-G01-W confirmation', async () => {
    const { installer, device } = await setup();
    await expect(installer.prepare(false)).rejects.toThrow('RDP-G01-W'); expect(device.connected).toBe(0); expect(device.writes).toEqual([]);
  });
  it('requires verified release object, not a plausible unsigned payload', async () => {
    const { release, device } = await setup(); expect(() => new Installer({ ...release }, device, () => {})).toThrow('签名');
  });
  it('verifies download hash before even connecting', async () => {
    const { release, device } = await setup();
    const installer = new Installer(release, device, () => {}, async () => new Uint8Array(32));
    await installer.prepare(true); expect(installer.stage).toBe('failed'); expect(device.connected).toBe(0); expect(device.writes).toEqual([]);
  });
  it('requires full-length backup, actual-file acknowledgement, SHA evidence and factory confirmation', async () => {
    const { installer, device } = await setup(); await installer.prepare(true);
    expect(installer.stage).toBe('awaiting-confirmation'); expect(installer.backup?.length).toBe(FLASH_SIZE); expect(installer.evidence?.sha256).toMatch(/^[a-f0-9]{64}$/);
    await expect(installer.install(true)).rejects.toThrow();
    for (const [binary, evidence, user] of [[false, true, true], [true, false, true], [true, true, false]]) expect(() => installer.confirmBackupSaved(installer.evidence!.sha256, binary, evidence, user)).toThrow();
    expect(() => installer.confirmBackupSaved('0'.repeat(64), true, true, true)).toThrow();
    installer.confirmBackupSaved(installer.evidence!.sha256, true, true, true);
    await expect(installer.install(false)).rejects.toThrow(); expect(device.writes).toEqual([]);
    await installer.cancel(); expect(device.closed).toBe(1);
  });
  it('writes only four parts, reads back each, preserves NVS/storage and closes before success', async () => {
    const { installer, device, save, updates } = await setup();
    const nvs = device.memory.slice(0x9000, 0x10000), storage = device.memory.slice(0x410000, 0x910000);
    await installer.prepare(true); save(); await installer.install(true);
    expect(installer.stage).toBe('success'); expect(device.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);
    expect(nvs.every((byte, i) => byte === device.memory[0x9000 + i])).toBe(true);
    expect(storage.every((byte, i) => byte === device.memory[0x410000 + i])).toBe(true);
    expect(device.closed).toBe(1); expect(installer.connected).toBe(false);
    expect(updates.filter(u => u.stage === 'verifying').length).toBeGreaterThanOrEqual(4);
    await expect(installer.install(true)).rejects.toThrow();
  });
  it('permits known official layout only after a separate migration authorization', async () => {
    const { installer, device, save } = await setup(true);
    device.memory.fill(0xff, 0x910000, 0xf20000);
    await installer.prepare(true); save();
    expect(installer.migration).toBe(true); await expect(installer.install(true)).rejects.toThrow('迁移');
    expect(device.writes).toEqual([]); await installer.install(true, true); expect(installer.stage).toBe('success');
  });
  it.each([0x910000, 0x911fff, 0x91ffff, 0x920000, 0xc20000, 0xf1ffff])('blocks even one occupied legacy tail byte at %i', async offset => {
    const { installer, device, save } = await setup(true);
    device.memory.fill(0xff, 0x910000, 0xf20000); device.memory[offset] = 0xfe;
    await installer.prepare(true); save();
    expect(installer.evidence?.layout).toContain('已有数据'); expect(installer.canInstall).toBe(false);
    await expect(installer.install(true, true)).rejects.toThrow(); expect(device.writes).toEqual([]);
    await installer.cancel();
  });
  it('rechecks the new OTA region before the very first write', async () => {
    const { installer, device, save } = await setup(true);
    device.memory.fill(0xff, 0x910000, 0xf20000);
    await installer.prepare(true); save(); device.memory[0xf1ffff] = 0;
    await installer.install(true, true); expect(installer.stage).toBe('failed'); expect(device.writes).toEqual([]);
  });
  it('allows the early 2 MiB factory layout only with the original SPIFFS and all affected space erased', async () => {
    const { installer, device, save } = await setup('spiffs');
    device.memory.fill(0xff, 0x210000, 0xf20000);
    expect(device.memory[0x20ffff]).toBe(0xa5); // Occupied legacy factory itself is allowed.
    const ranges: number[][] = []; device.onMd5 = (at, n) => ranges.push([at, n]);
    await installer.prepare(true); save(); expect(installer.layoutLabel).toContain('SPIFFS');
    await expect(installer.install(true)).rejects.toThrow(); expect(device.writes).toEqual([]);
    await installer.install(true, true); expect(installer.stage).toBe('success');
    expect(ranges).toContainEqual([0x210000, 0xf20000 - 0x210000]);
    expect(device.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);
  });
  it.each([0x210000, 0x40ffff, 0x410000, 0x70ffff, 0x710000, 0xf1ffff])('refuses to repurpose legacy SPIFFS or unassigned bytes at %i', async offset => {
    const { installer, device, save } = await setup('spiffs');
    device.memory.fill(0xff, 0x210000, 0xf20000); device.memory[offset] = 0xfe;
    await installer.prepare(true); save(); expect(installer.layoutLabel).toContain('原 SPIFFS 与新版 factory/storage 重叠');
    expect(installer.compatible).toBe(false); await expect(installer.install(true, true)).rejects.toThrow();
    expect(device.writes).toEqual([]); await installer.cancel();
  });
  it('blocks a legacy SPIFFS byte that changed after the backup', async () => {
    const { installer, device, save } = await setup('spiffs'); device.memory.fill(0xff, 0x210000, 0xf20000);
    await installer.prepare(true); save(); device.memory[0x210000] = 0;
    await installer.install(true, true); expect(installer.stage).toBe('failed'); expect(device.writes).toEqual([]);
  });
  it('rejects an incomplete device digest instead of treating it as blank', async () => {
    const { installer, device, save } = await setup(true);
    device.memory.fill(0xff, 0x910000, 0xf20000);
    await installer.prepare(true); save();
    device.md5 = async () => '';
    await installer.install(true, true); expect(installer.stage).toBe('failed'); expect(device.writes).toEqual([]);
  });
  it('rejects collateral storage changes before reporting success', async () => {
    const { installer, device, save } = await setup();
    await installer.prepare(true); save();
    device.onMd5 = offset => { if (offset === 0x410000 && device.writes.length) device.memory[0x90ffff] ^= 1; };
    await installer.install(true); expect(installer.stage).toBe('failed');
  });
  it('migrates the known ThingsOS layout only after full backup and explicit replacement consent', async () => {
    const { installer, device, save } = await setup('thingsos');
    await installer.prepare(true); save();
    expect(installer.migration).toBe(true); expect(installer.compatible).toBe(true);
    await expect(installer.install(true)).rejects.toThrow('迁移');
    expect(device.writes).toEqual([]);
    const ranges: number[][] = []; device.onMd5 = (at, n) => ranges.push([at, n]);
    await installer.install(true, true);
    expect(installer.stage).toBe('success'); expect(ranges).toContainEqual([0, FLASH_SIZE]);
    expect(device.writes).toEqual([0, 0x8000, 0x10000, 0x910000]);
  });
  it('blocks ThingsOS migration if the device changes after the saved backup', async () => {
    const { installer, device, save } = await setup('thingsos');
    await installer.prepare(true); save(); device.memory[0xd00000] ^= 1;
    await installer.install(true, true); expect(installer.stage).toBe('failed'); expect(device.writes).toEqual([]);
  });
  it('allows backup but never writes unknown layout or unvalidated 16MiB flash vendor', async () => {
    for (const unknown of ['layout', 'vendor']) {
      const { installer, device, save } = await setup();
      if (unknown === 'layout') device.memory.fill(0xff, 0x8000, 0x9000); else device.info.flashId = 0x1840ef;
      await installer.prepare(true); expect(installer.backup?.length).toBe(FLASH_SIZE); expect(installer.compatible).toBe(false);
      save(); await expect(installer.install(true, true)).rejects.toThrow(); expect(device.writes).toEqual([]); await installer.cancel(); expect(device.closed).toBe(1);
    }
  });
  it.each(['chip', 'flashSize', 'securitySafe'])('blocks mismatched %s and closes', async key => {
    const { installer, device } = await setup();
    Object.assign(device.info, { [key]: key === 'chip' ? 'ESP32' : key === 'flashSize' ? 0x800000 : false });
    await installer.prepare(true); expect(installer.stage).toBe('failed'); expect(device.writes).toEqual([]); expect(device.closed).toBe(1);
  });
  it('refuses partial backups and never produces evidence', async () => {
    const { installer, device } = await setup(); device.shortBackup = true; await installer.prepare(true);
    expect(installer.stage).toBe('failed'); expect(installer.evidence).toBeUndefined(); expect(device.writes).toEqual([]); expect(device.closed).toBe(1);
  });
  it('rejects a changed partition table after confirmation but before writing', async () => {
    const { installer, device, save } = await setup(); await installer.prepare(true); save(); device.memory[0x8000] ^= 1;
    await installer.install(true); expect(installer.stage).toBe('failed'); expect(device.writes).toEqual([]); expect(device.closed).toBe(1);
  });
  it.each(['writeFailure', 'corruptReadback', 'closeFailure'])('never reports success on %s', async failure => {
    const { installer, device, save, updates } = await setup(); await installer.prepare(true); save();
    Object.assign(device, { [failure]: true }); await installer.install(true);
    expect(installer.stage).toBe('failed'); expect(updates.some(u => u.stage === 'success')).toBe(false); expect(device.closed).toBe(1);
  });
  it.each(['failed', 'cancelled', 'after-write'])('reports a hung close on %s without claiming success', async path => {
    const { installer, device, save, updates } = await setup();
    const close = vi.spyOn(device, 'close').mockImplementation(() => new Promise<void>(() => {}));
    if (path === 'failed') { device.info.securitySafe = false; await installer.prepare(true); }
    else { await installer.prepare(true); if (path === 'cancelled') await installer.cancel(); else { save(); await installer.install(true); } }
    expect(installer.stage).toBe(path === 'cancelled' ? 'cancelled' : 'failed');
    expect(updates.some(u => u.stage === 'success')).toBe(false); expect(close).toHaveBeenCalledTimes(1);
    expect(updates.at(-1)?.message).toContain('串口关闭失败');
    if (path !== 'after-write') expect(device.writes).toEqual([]);
  }, 7000);
  it('cancels during backup with no writes and closes exactly once', async () => {
    const { installer, device } = await setup();
    device.onRead = (_offset, size) => { if (size === FLASH_SIZE) void installer.cancel(); };
    await installer.prepare(true); await installer.cancel();
    expect(installer.stage).toBe('cancelled'); expect(device.writes).toEqual([]); expect(device.closed).toBe(1); expect(installer.canInstall).toBe(false);
  });
  it('cancellation during readback cannot become success', async () => {
    const { installer, device, save, updates } = await setup(); await installer.prepare(true); save();
    device.onRead = () => { if (device.writes.length) void installer.cancel(); };
    await installer.install(true); expect(installer.stage).toBe('cancelled'); expect(device.writes).toEqual([0]);
    expect(device.closed).toBe(1); expect(updates.some(u => u.stage === 'success')).toBe(false);
  });
});
