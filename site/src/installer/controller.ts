import { assertVerified, FLASH_SIZE, sha256, validateImage, fetchWithMirror, type Release, type Part } from './manifest';
import { assessInstalledLayout, type LayoutAssessment } from './partition';

export interface DeviceInfo { chip: string; flashSize: number; flashId: number; securitySafe: boolean }
export const SUPPORTED_FLASH_ID = 0x18405e; // SPI RDID little-endian: manufacturer 0x5e, type 0x40, capacity 0x18.
export interface SerialDevice {
  connect(signal: AbortSignal): Promise<DeviceInfo>;
  read(offset: number, size: number, signal: AbortSignal, progress: (bytes: number) => void): Promise<Uint8Array>;
  md5(offset: number, size: number, signal: AbortSignal): Promise<string>;
  write(part: Part, bytes: Uint8Array, signal: AbortSignal, progress: (bytes: number, total: number) => void): Promise<void>;
  close(): Promise<void>;
}
export type Stage = 'idle' | 'downloading' | 'connecting' | 'backing-up' | 'awaiting-confirmation' | 'writing' | 'verifying' | 'success' | 'failed' | 'cancelled';
export interface Update { stage: Stage; message: string; done?: number; total?: number }
export interface BackupEvidence {
  schema: 1; kind: 'folio-local-flash-backup'; size: number; sha256: string; createdAt: string;
  chip: string; hardware: 'RDP-G01-W (user confirmed)'; layout: string;
  release: { version: string; sequence: number; channel: string }; privacy: string;
}
export class Installer {
  #stage: Stage = 'idle';
  #abort = new AbortController();
  #backup?: Uint8Array;
  #evidence?: BackupEvidence;
  #layout?: LayoutAssessment;
  #supportedFlash = false;
  #images: Uint8Array[] = [];
  #saved = false;
  #open = false;
  #wrote = false;
  #cancelPromise?: Promise<void>;
  #closePromise?: Promise<void>;
  constructor(private release: Release, private device: SerialDevice, private emit: (update: Update) => void,
    private download = fetchWithMirror) { assertVerified(release); }
  get stage(): Stage { return this.#stage; }
  get connected(): boolean { return this.#open; }
  get compatible(): boolean { return this.#layout?.compatible === true && this.#supportedFlash; }
  get migration(): boolean { return this.#layout?.migration === true; }
  get layoutLabel(): string { return this.#layout?.label ?? '尚未读取布局'; }
  get canInstall(): boolean { return this.#stage === 'awaiting-confirmation' && this.compatible && this.#saved && !this.#abort.signal.aborted; }
  get backup(): Uint8Array | undefined { return this.#backup?.slice(); }
  get evidence(): BackupEvidence | undefined { return this.#evidence ? structuredClone(this.#evidence) : undefined; }
  #status(stage: Stage, message: string, done?: number, total?: number): void {
    if (this.#abort.signal.aborted && stage !== 'cancelled' && stage !== 'failed') return;
    this.#stage = stage; this.emit({ stage, message, done, total });
  }
  async prepare(physicalModelConfirmed: boolean): Promise<void> {
    if (this.#stage !== 'idle') throw new Error('不能重复开启安装会话。');
    if (!physicalModelConfirmed) throw new Error('必须先根据机身/购买凭据确认 RDP-G01-W，不能只看 ESP32-S3。');
    try {
      const signal = this.#abort.signal;
      const total = this.release.parts.reduce((n, part) => n + part.size, 0);
      let downloaded = 0;
      for (const part of this.release.parts) {
        signal.throwIfAborted();
        this.#status('downloading', '下载并校验四段镜像；此时不写设备。', downloaded, total);
        const bytes = await this.download(part.url, part.size, signal, n => this.#status('downloading', '下载签名镜像', downloaded + n, total));
        await validateImage(part, bytes, this.release); this.#images.push(bytes); downloaded += bytes.length;
      }
      signal.throwIfAborted();
      this.#status('connecting', '请选择已核对的设备；连接会进入下载模式，但尚不写 Flash。');
      this.#open = true; // Close even if connection fails partway through.
      const info = await this.device.connect(signal);
      if (info.chip !== 'ESP32-S3' || info.flashSize !== FLASH_SIZE || !info.securitySafe) throw new Error('芯片/16MiB Flash/安全熔丝状态不匹配，禁止写入。');
      this.#supportedFlash = info.flashId === SUPPORTED_FLASH_ID;
      signal.throwIfAborted();
      const table = await this.device.read(0x8000, 4096, signal, () => {});
      this.#layout = assessInstalledLayout(table);
      if (!this.#supportedFlash) this.#layout = { ...this.#layout, compatible: false, label: `Flash RDID 0x${info.flashId.toString(16)} 尚不支持当前 120MHz profile；仅允许备份。${this.#layout.label}` };
      this.#status('backing-up', '正在读取完整 16MiB Flash；备份可能含凭据，请妥善保管。', 0, FLASH_SIZE);
      const backup = await this.device.read(0, FLASH_SIZE, signal, n => this.#status('backing-up', '读取完整 Flash 备份', n, FLASH_SIZE));
      if (backup.length !== FLASH_SIZE) throw new Error('备份长度不是完整 16MiB。');
      if (!table.every((b, i) => b === backup[0x8000 + i])) throw new Error('备份中的分区表与连接时读取结果不同。');
      signal.throwIfAborted();
      const digest = await sha256(backup);
      signal.throwIfAborted();
      this.#backup = backup;
      this.#evidence = { schema: 1, kind: 'folio-local-flash-backup', size: FLASH_SIZE, sha256: digest, createdAt: new Date().toISOString(), chip: info.chip,
        hardware: 'RDP-G01-W (user confirmed)', layout: this.#layout.label,
        release: { version: this.release.version, sequence: this.release.sequence, channel: this.release.channel },
        privacy: 'Contains private internal Flash data. Keep locally; never upload to issues or public releases.' };
      this.#status('awaiting-confirmation', this.compatible ? '完整备份已读取。请分别保存备份与 SHA 证据，再确认重装风险。' : '完整备份已读取，但当前布局不受支持。只允许下载备份；网页禁止迁移/烧录。');
    } catch (error) { await this.#fail(error); }
  }
  confirmBackupSaved(digest: string, binaryDownloaded: boolean, evidenceDownloaded: boolean, userConfirmed: boolean): void {
    if (this.#stage !== 'awaiting-confirmation' || !this.#evidence || digest !== this.#evidence.sha256 || !binaryDownloaded || !evidenceDownloaded || !userConfirmed) throw new Error('请实际保存完整备份和 SHA-256 证据，并明确确认。');
    this.#saved = true;
  }
  async install(factoryResetConfirmed: boolean, migrationConfirmed = false): Promise<void> {
    if (!this.canInstall || !factoryResetConfirmed || (this.migration && !migrationConfirmed)) throw new Error('缺少备份确认、布局校验、迁移授权或切换 factory 的授权。');
    this.#status('verifying', '重新检查所有镜像与设备布局；尚未写入。');
    try {
      const signal = this.#abort.signal;
      // Revalidate every image before the first destructive command.
      for (let i = 0; i < this.release.parts.length; i++) await validateImage(this.release.parts[i], this.#images[i], this.release);
      const table = await this.device.read(0x8000, 4096, signal, () => {});
      if (!table.every((b, i) => b === this.#backup![0x8000 + i]) || table.length !== 4096) throw new Error('分区表变化，请重新完整备份。');
      for (let i = 0; i < this.release.parts.length; i++) {
        const part = this.release.parts[i]; signal.throwIfAborted();
        this.#wrote = true;
        this.#status('writing', `写入 ${i + 1}/4 · 0x${part.offset.toString(16)}，勿断电。`, 0, part.size);
        await this.device.write(part, this.#images[i], signal, (n, total) => this.#status('writing', `写入 ${i + 1}/4 · 0x${part.offset.toString(16)}（传输字节，不代表验收）`, n, total));
        signal.throwIfAborted();
        this.#status('verifying', `回读并验证 ${i + 1}/4 · SHA-256`, 0, part.size);
        const actual = await this.device.read(part.offset, part.size, signal, n => this.#status('verifying', `回读并验证 ${i + 1}/4 · SHA-256`, n, part.size));
        if (actual.length !== part.size || await sha256(actual) !== part.sha256) throw new Error(`0x${part.offset.toString(16)} 回读校验失败，不能确认安装成功。`);
      }
      // Protect the legacy NVS + PHY + gap explicitly, not only through address math.
      this.#status('verifying', '核对 NVS 与 PHY 保留区未被改写', 0, 0x7000);
      const preserved = await this.device.read(0x9000, 0x7000, signal, n => this.#status('verifying', '核对 NVS 与 PHY 保留区', n, 0x7000));
      if (preserved.length !== 0x7000 || !preserved.every((b, i) => b === this.#backup![0x9000 + i])) throw new Error('NVS/PHY 保留区与备份不符，停止并按恢复指南处理。');
      signal.throwIfAborted();
      await this.#close();
      this.#status('success', '四段写入与回读 SHA-256 全部通过，NVS 保留区一致，串口已关闭。请按设备说明重新启动；这不代表硬件功能验收。');
    } catch (error) { await this.#fail(error); }
  }
  async #close(): Promise<void> {
    if (!this.#open) return;
    this.#closePromise ??= this.device.close().then(() => { this.#open = false; });
    await this.#closePromise;
  }
  async #fail(error: unknown): Promise<void> {
    const message = error instanceof Error ? error.message : '未知错误';
    let closeError = '';
    try { await this.#close(); } catch { closeError = ' 串口关闭失败，请关闭此页后按官方说明重连。'; }
    this.#status(this.#abort.signal.aborted ? 'cancelled' : 'failed', `${this.#abort.signal.aborted ? '已取消。' : message}${this.#wrote ? ' 写入可能不完整，禁止当作成功；请保留备份并查看恢复指南。' : ' 未执行 Flash 写入。'}${closeError}`);
  }
  async cancel(): Promise<void> {
    if (this.#cancelPromise) return this.#cancelPromise;
    this.#abort.abort(new DOMException('用户取消', 'AbortError'));
    this.#cancelPromise = this.#fail(new Error('已取消'));
    return this.#cancelPromise;
  }
}
