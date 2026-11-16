import { describe, it, expect, vi, beforeEach } from 'vitest';
const state = vi.hoisted(() => ({ writes: [] as unknown[], closed: 0, chip: 'ESP32-S3', flashId: 0x18405e, secure: false, crypt: 0, resets: [] as string[], attempts: undefined as number | undefined, detectWait: undefined as Promise<void> | undefined, serialOptions: undefined as { bufferSize?: number } | undefined, md5Calls: [] as number[][] }));
vi.mock('esptool-js', () => ({
  Transport: class { async disconnect() { state.closed++; } },
  ESPLoader: class {
    constructor(options: { serialOptions?: { bufferSize?: number } }) { state.serialOptions = options.serialOptions; }
    chip = { CHIP_NAME: state.chip, postConnect: async () => {} };
    IS_STUB = true;
    async detectChip(mode: string, attempts?: number) { state.resets.push(mode); state.attempts = attempts; await state.detectWait; }
    async getSecurityInfo() { return { flashCryptCnt: state.crypt, parsedFlags: { SECURE_BOOT_EN: state.secure, SECURE_DOWNLOAD_ENABLE: false } }; }
    async runStub() {}
    async readFlashId() { return state.flashId; }
    async flashMd5sum(offset: number, size: number) { state.md5Calls.push([offset, size]); return 'ab'.repeat(16); }
    async writeFlash(options: unknown) { state.writes.push(options); }
  },
}));
import { BrowserSerialDevice } from '../src/installer/serial';
import { fixture } from './fixtures';
const signal = () => new AbortController().signal;
beforeEach(() => { state.writes = []; state.closed = 0; state.chip = 'ESP32-S3'; state.flashId = 0x18405e; state.secure = false; state.crypt = 0; state.resets = []; state.attempts = undefined; state.detectWait = undefined; state.serialOptions = undefined; state.md5Calls = []; });
describe('esptool-js adapter contract (mock library, never a USB device)', () => {
  it('keeps signed headers unchanged, never enables eraseAll and uses device MD5', async () => {
    const f = await fixture(), device = new BrowserSerialDevice({} as SerialPort, 'usb_reset');
    expect(await device.connect(signal())).toMatchObject({ chip: 'ESP32-S3', flashSize: 16777216, flashId: 0x18405e });
    expect(await device.md5(0x9000, 0x5000, signal())).toBe('ab'.repeat(16));
    expect(state.md5Calls).toEqual([[0x9000, 0x5000]]);
    await device.write(f.release.parts[0], f.images[0], signal(), () => {});
    expect(state.resets).toEqual(['usb_reset']);
    expect(state.attempts).toBeUndefined();
    expect(state.serialOptions).toEqual({ bufferSize: 8192 });
    expect(state.writes).toHaveLength(1);
    expect(state.writes[0]).toMatchObject({ flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep', eraseAll: false, compress: true, fileArray: [{ address: 0, data: f.images[0] }] });
    await device.close(); await device.close(); expect(state.closed).toBe(1);
    await expect(device.write(f.release.parts[0], f.images[0], signal(), () => {})).rejects.toThrow('门禁');
    await expect(device.md5(0x1000000, 1, signal())).rejects.toThrow('范围');
  });
  it('rejects writes before connection and to NVS/resources even after connection', async () => {
    const f = await fixture(), device = new BrowserSerialDevice({} as SerialPort, 'default_reset');
    await expect(device.write(f.release.parts[0], f.images[0], signal(), () => {})).rejects.toThrow('门禁');
    await device.connect(signal());
    for (const offset of [0x9000, 0x410000, 0x920000]) await expect(device.write({ ...f.release.parts[0], offset }, f.images[0], signal(), () => {})).rejects.toThrow('门禁');
    expect(state.writes).toEqual([]);
  });
  it('reads but refuses write for another 16MiB flash vendor', async () => {
    const f = await fixture(); state.flashId = 0x1840ef;
    const device = new BrowserSerialDevice({} as SerialPort, 'no_reset');
    expect(await device.connect(signal())).toMatchObject({ flashSize: 16777216, flashId: 0x1840ef });
    await expect(device.write(f.release.parts[0], f.images[0], signal(), () => {})).rejects.toThrow('门禁'); expect(state.writes).toEqual([]);
  });
  it('closes after an in-flight port-open/reset settles when cancelled', async () => {
    let finish!: () => void;
    state.detectWait = new Promise<void>(resolve => { finish = resolve; });
    const abort = new AbortController(), device = new BrowserSerialDevice({} as SerialPort, 'default_reset');
    const connecting = device.connect(abort.signal);
    abort.abort(); const closing = device.close();
    expect(state.closed).toBe(0);
    const rejected = expect(connecting).rejects.toThrow();
    finish(); await rejected; await closing;
    expect(state.closed).toBe(1); expect(state.writes).toEqual([]);
  });
  it.each(['chip', 'secure', 'crypt'])('rejects unsafe %s before running a stub', async kind => {
    if (kind === 'chip') state.chip = 'ESP32'; else if (kind === 'secure') state.secure = true; else state.crypt = 1;
    const device = new BrowserSerialDevice({} as SerialPort, 'default_reset');
    await expect(device.connect(signal())).rejects.toThrow(); expect(state.writes).toEqual([]);
  });
});
