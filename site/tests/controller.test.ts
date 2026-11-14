import { describe, it, expect } from 'vitest';
import { Installer, type Update } from '../src/installer/controller';
import { FLASH_SIZE } from '../src/installer/manifest';
import { FakeDevice, fixture } from './fixtures';

async function setup(official = false) {
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
    const { installer, device, save } = await setup(true); await installer.prepare(true); save();
    expect(installer.migration).toBe(true); await expect(installer.install(true)).rejects.toThrow('迁移');
    expect(device.writes).toEqual([]); await installer.install(true, true); expect(installer.stage).toBe('success');
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
