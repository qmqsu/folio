import { md5 } from '@noble/hashes/legacy.js';
import { FLASH_SIZE, assertVerified, fetchWithMirror, hex, validateImage, type Release } from './manifest';
import { assessInstalledLayout, supportsReplacement } from './partition';
import { SUPPORTED_FLASH_ID, type SerialDevice } from './controller';

export const CLOSE_TIMEOUT_MS = 2000;
export type QuickStage = 'idle' | 'downloading' | 'connecting' | 'writing' | 'verifying' | 'success' | 'failed' | 'cancelled';
export interface QuickUpdate { stage: QuickStage; message: string; done?: number; total?: number }

class UnsupportedDevice extends Error {}

/** Signed four-segment install with on-device MD5 verification, not bulk backup. */
export class QuickFlash {
  #stage: QuickStage = 'idle';
  #abort = new AbortController();
  #images: Uint8Array[] = [];
  #open = false;
  #wrote = false;
  #closing?: Promise<void>;

  constructor(private release: Release, private device: SerialDevice, private emit: (update: QuickUpdate) => void,
    private download = fetchWithMirror) {
    assertVerified(release);
  }

  get stage(): QuickStage { return this.#stage; }
  get connected(): boolean { return this.#open; }

  #status(stage: QuickStage, message: string, done?: number, total?: number): void {
    if (this.#abort.signal.aborted && stage !== 'failed' && stage !== 'cancelled') return;
    this.#stage = stage;
    this.emit({ stage, message, done, total });
  }

  async run(saveBackup?: (bytes: Uint8Array) => Promise<boolean>, reconnect?: () => SerialDevice): Promise<void> {
    if (this.#stage !== 'idle') throw new Error('一次只能刷一台设备，请刷新页面后重试。');
    try {
      const signal = this.#abort.signal;
      const total = this.release.parts.reduce((sum, part) => sum + part.size, 0);
      let downloaded = 0;
      for (const [index, part] of this.release.parts.entries()) {
        signal.throwIfAborted();
        this.#status('downloading', `下载第 ${index + 1}/4 段镜像`, downloaded, total);
        const bytes = await this.download(part.url, part.size, signal, n => this.#status('downloading', `下载第 ${index + 1}/4 段镜像`, downloaded + n, total));
        await validateImage(part, bytes, this.release);
        this.#images.push(bytes);
        downloaded += bytes.length;
      }
      signal.throwIfAborted();

      this.#status('connecting', '连接设备，读取芯片与分区表（最多等 30 秒）');
      this.#open = true;
      const inspect = async () => {
        const info = await this.device.connect(signal);
        if (info.chip !== 'ESP32-S3' || info.flashSize !== FLASH_SIZE || !info.securitySafe) {
          throw new UnsupportedDevice('芯片、Flash 容量或安全状态不匹配，已停止。');
        }
        if (info.flashId !== SUPPORTED_FLASH_ID) throw new UnsupportedDevice('这台机器的 Flash 料号还没验证过，一键刷不写。');
        return this.device.read(0x8000, 4096, signal, () => {});
      };
      let table: Uint8Array;
      try {
        table = await inspect();
      } catch (error) {
        if (!reconnect || signal.aborted || error instanceof UnsupportedDevice) throw error;
        await this.#close();
        signal.throwIfAborted();
        this.#closing = undefined;
        this.device = reconnect();
        this.#open = true;
        this.#status('connecting', '改用原生 USB 复位，重新识别设备并读取分区表');
        table = await inspect();
      }
      const layout = assessInstalledLayout(table);
      if (!layout.compatible || layout.migration) {
        if (!saveBackup || !supportsReplacement(table)) throw new Error('未能确认原系统的分区表。请在设备进入 BOOT 下载模式后重试，或使用下方备份工具。');
        this.#status('verifying', '检测到其他系统，正在读取完整的 16 MiB 恢复备份', 0, FLASH_SIZE);
        const backup = await this.device.read(0, FLASH_SIZE, signal,
          n => this.#status('verifying', '读取原系统备份', n, FLASH_SIZE));
        const original = hex(md5(backup));
        if (backup.length !== FLASH_SIZE || !table.every((byte,i) => byte === backup[0x8000+i]) ||
            original !== await this.device.md5(0, FLASH_SIZE, signal)) throw new Error('原系统备份与设备内容不一致。');
        if (!await saveBackup(backup)) throw new Error('已取消替换原系统。');
        if (await this.device.md5(0, FLASH_SIZE, signal) !== original) throw new Error('保存备份后设备内容已变化，请重新连接。');
      }
      signal.throwIfAborted();

      // Device-side hashes prove preservation without downloading private flash contents.
      const protectedRegions = [[0x9000, 0x7000, 'NVS/PHY'], [0x410000, 0x500000, '字体资源区']] as const;
      const preserved = new Map<number, string>();
      for (const [offset, size, name] of protectedRegions) {
        this.#status('verifying', `记录${name}的设备端校验值`);
        preserved.set(offset, await this.device.md5(offset, size, signal));
      }

      for (const [index, part] of this.release.parts.entries()) {
        signal.throwIfAborted();
        this.#wrote = true;
        this.#status('writing', `写入第 ${index + 1}/4 段`, 0, part.size);
        await this.device.write(part, this.#images[index], signal, (n, size) => this.#status('writing', `写入第 ${index + 1}/4 段`, n, size));
        signal.throwIfAborted();
        this.#status('verifying', `设备端 MD5 校验第 ${index + 1}/4 段`);
        const actual = await this.device.md5(part.offset, part.size, signal);
        if (actual !== hex(md5(this.#images[index]))) throw new Error('设备端校验和签名镜像对不上，已经停手。');
      }

      for (const [offset, size, name] of protectedRegions) {
        this.#status('verifying', `确认${name}未被写入`);
        if (await this.device.md5(offset, size, signal) !== preserved.get(offset)) {
          throw new Error(`${name}与写入前校验值不一致，已停止。`);
        }
      }

      this.#status('verifying', '全部校验通过，正在重启设备');
      await this.device.reset(signal);
      try {
        await this.#close();
      } catch {
        this.#status('success', '四段镜像与保护区校验通过，设备已重启。浏览器未确认串口关闭，请关闭此页后再连接。');
        return;
      }
      this.#status('success', '四段写入与设备端 MD5 校验全部通过，NVS/PHY 和字体资源区未变。已重启，等它亮屏后再拔线。');
    } catch (error) {
      await this.#fail(error);
    }
  }

  async cancel(): Promise<void> {
    this.#abort.abort(new DOMException('用户取消', 'AbortError'));
    await this.#fail(new Error('已取消'));
  }

  async #close(): Promise<void> {
    if (!this.#open) return;
    this.#closing ??= this.device.close().then(() => { this.#open = false; });
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
      await Promise.race([
        this.#closing,
        new Promise<never>((_, reject) => { timer = setTimeout(() => reject(new Error('串口关闭超时')), CLOSE_TIMEOUT_MS); }),
      ]);
    } finally { if (timer) clearTimeout(timer); }
  }

  async #fail(error: unknown): Promise<void> {
    let message = error instanceof Error ? error.message : '未知错误';
    if (message.includes('No serial data received')) message += '。请选择 USB JTAG_serial debug unit，关闭其他串口程序，短按电源键唤醒，或按机型说明进入 BOOT 模式后重试。';
    let closeNote = '';
    try { await this.#close(); }
    catch { closeNote = ' 串口没关掉，关掉这个页面再重连。'; }
    const cancelled = this.#abort.signal.aborted;
    this.#stage = cancelled ? 'cancelled' : 'failed';
    this.emit({
      stage: this.#stage,
      message: `${cancelled ? '已取消。' : message}${this.#wrote ? ' 可能只写了一部分，别当成功；按刷机说明里的恢复步骤来。' : ' 没有写入任何东西。'}${closeNote}`
    });
  }
}
