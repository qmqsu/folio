import { FLASH_SIZE, assertVerified, fetchWithMirror, sha256, validateImage, type Release } from './manifest';
import { assessInstalledLayout } from './partition';
import { SUPPORTED_FLASH_ID, type DeviceInfo, type SerialDevice } from './controller';

export type QuickStage = 'idle' | 'downloading' | 'connecting' | 'backing-up' | 'writing' | 'verifying' | 'success' | 'failed' | 'cancelled';
export interface FlashOptions { backup?: boolean; onBackup?: (backup: { bytes: Uint8Array; sha256: string }) => void }
export interface QuickUpdate { stage: QuickStage; message: string; done?: number; total?: number }

/**
 * One-click flashing: verify the signed release, check the device, write the four
 * whitelisted segments and read each one back. It always reads a fresh full
 * 16MiB backup before writing and rejects migration or unknown layouts.
 */
export class QuickFlash {
  #stage: QuickStage = 'idle';
  #abort = new AbortController();
  #images: Uint8Array[] = [];
  #open = false;
  #wrote = false;
  #closed = false;
  #backup?: Uint8Array;

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

  async run(options: FlashOptions = {}): Promise<void> {
    if (this.#stage !== 'idle') throw new Error('一次只能刷一台设备，请刷新页面后重试。');
    try {
      const signal = this.#abort.signal;
      if (options.backup === false) throw new Error('完整 16 MiB 备份是写入前的必要步骤，不能跳过。');
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
      const info: DeviceInfo = await this.device.connect(signal);
      if (info.chip !== 'ESP32-S3' || info.flashSize !== FLASH_SIZE || !info.securitySafe) {
        throw new Error('芯片、Flash 容量或安全状态不匹配，已停止。');
      }
      if (info.flashId !== SUPPORTED_FLASH_ID) throw new Error('这台机器的 Flash 料号还没验证过，一键刷不写。');
      const table = await this.device.read(0x8000, 4096, signal, () => {});
      const layout = assessInstalledLayout(table);
      if (!layout.compatible || layout.migration) {
        throw new Error(layout.migration ? '这台还是原厂四分区布局，一键刷不会替你改分区表。' : '分区布局不认识，一键刷不写。');
      }
      signal.throwIfAborted();

      this.#status('backing-up', '读取整片 16 MiB，别拔线', 0, FLASH_SIZE);
      this.#backup = await this.device.read(0, FLASH_SIZE, signal, n => this.#status('backing-up', '读取整片 16 MiB，别拔线', n, FLASH_SIZE));
      if (this.#backup.length !== FLASH_SIZE) throw new Error('备份不是完整的 16 MiB，已停止。');
      const digest = await sha256(this.#backup);
      signal.throwIfAborted();
      options.onBackup?.({ bytes: this.#backup, sha256: digest });
      this.#status('backing-up', `备份已读出（SHA-256 ${digest.slice(0, 12)}…）`, FLASH_SIZE, FLASH_SIZE);

      for (const [index, part] of this.release.parts.entries()) {
        signal.throwIfAborted();
        this.#wrote = true;
        this.#status('writing', `写入第 ${index + 1}/4 段`, 0, part.size);
        await this.device.write(part, this.#images[index], signal, (n, size) => this.#status('writing', `写入第 ${index + 1}/4 段`, n, size));
        signal.throwIfAborted();
        this.#status('verifying', `回读第 ${index + 1}/4 段`, 0, part.size);
        const actual = await this.device.read(part.offset, part.size, signal, n => this.#status('verifying', `回读第 ${index + 1}/4 段`, n, part.size));
        if (actual.length !== part.size || await sha256(actual) !== part.sha256) throw new Error('回读内容和签名对不上，已经停手。');
      }

      for (const [offset, size, name] of [[0x9000, 0x5000, 'NVS/PHY'], [0x410000, 0x500000, '字体资源区']] as const) {
        this.#status('verifying', `确认${name}未被写入`, 0, size);
        const actual = await this.device.read(offset, size, signal, n => this.#status('verifying', `确认${name}未被写入`, n, size));
        const expected = this.#backup!.subarray(offset, offset + size);
        if (actual.length !== size || !actual.every((byte, index) => byte === expected[index])) {
          throw new Error(`${name}与写入前备份不一致，已停止。`);
        }
      }

      await this.#close();
      this.#status('success', '四段都写完并回读通过，NVS/PHY 和字体资源区未变。设备会自己重启，拔线前等它亮屏。');
    } catch (error) {
      await this.#fail(error);
    }
  }

  async cancel(): Promise<void> {
    this.#abort.abort(new DOMException('用户取消', 'AbortError'));
    await this.#fail(new Error('已取消'));
  }

  async #close(): Promise<void> {
    if (!this.#open || this.#closed) return;
    this.#closed = true;
    await this.device.close();
    this.#open = false;
  }

  async #fail(error: unknown): Promise<void> {
    const message = error instanceof Error ? error.message : '未知错误';
    let closeNote = '';
    try { await this.#close(); } catch { closeNote = ' 串口没关掉，关掉这个页面再重连。'; }
    const cancelled = this.#abort.signal.aborted;
    this.#stage = cancelled ? 'cancelled' : 'failed';
    this.emit({
      stage: this.#stage,
      message: `${cancelled ? '已取消。' : message}${this.#wrote ? ' 可能只写了一部分，别当成功；按刷机说明里的恢复步骤来。' : ' 没有写入任何东西。'}${closeNote}`
    });
  }
}
