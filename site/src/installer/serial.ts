import { ESPLoader, Transport } from 'esptool-js';
import { md5 } from '@noble/hashes/legacy.js';
import { FLASH_SIZE, hex, sha256, type Part } from './manifest';
import { SUPPORTED_FLASH_ID, type SerialDevice, type DeviceInfo } from './controller';

// Bounded so a browser whose control lines never reach the chip fails loudly.
export const CONNECT_TIMEOUT_MS = 30000;

export function browserSupport(): { supported: boolean; reason: string } {
  const desktop = !/Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent) && !(navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const chromeOrEdge = /(?:Chrome|Edg)\//.test(navigator.userAgent) && !/OPR\//.test(navigator.userAgent);
  if (!isSecureContext) return { supported: false, reason: '请使用 HTTPS 页面；USB 安装需要安全上下文。' };
  if (!desktop || !chromeOrEdge || !('serial' in navigator)) return { supported: false, reason: 'USB 安装需要桌面版 Chrome 或 Edge 及 Web Serial。手机、Safari、Firefox 请使用设备 OTA 或手动指南。' };
  if (!crypto.subtle) return { supported: false, reason: '此浏览器无法验证发布签名，已禁用安装。' };
  return { supported: true, reason: '桌面 Web Serial 可用。网站不会自动连接设备。部分 macOS 浏览器无法用 DTR/RTS 让原生 USB-JTAG 进入下载模式，连接失败时请换 Chrome 或使用手动方式。' };
}
interface StubTransport { read(timeout: number): Promise<Uint8Array>; write(data: Uint8Array): Promise<void> }
interface StubLoader {
  IS_STUB: boolean; ESP_READ_FLASH: number; FLASH_READ_TIMEOUT: number; transport: StubTransport;
  checkCommand(description: string, op: number, data: Uint8Array): Promise<number | Uint8Array>;
}
// Small bounded reads use the same 512-byte, one-frame flow as kiikoread.
// Large image/protected-region verification uses the device MD5 command instead.
export async function readStubFlash(loader: StubLoader, offset: number, size: number, signal: AbortSignal, progress: (bytes: number) => void): Promise<Uint8Array> {
  signal.throwIfAborted();
  if (!loader.IS_STUB || !Number.isInteger(offset) || !Number.isInteger(size) || offset < 0 || size <= 0 || offset + size > FLASH_SIZE) throw new Error('只允许 stub 模式下的有界 Flash 读取。');
  const request = new Uint8Array(16), view = new DataView(request.buffer);
  const packetSize = size <= 4096 ? 64 : 512;
  [offset, size, packetSize, 1].forEach((n, i) => view.setUint32(i * 4, n, true));
  const result = await loader.checkCommand('read flash', loader.ESP_READ_FLASH, request);
  if (result !== 0) throw new Error('设备拒绝读取 Flash。');
  const output = new Uint8Array(size);
  let received = 0, emptyFrames = 0;
  while (received < size) {
    signal.throwIfAborted();
    const packet = await loader.transport.read(5000);
    signal.throwIfAborted();
    if (packet.length === 0) {
      if (++emptyFrames > 2) throw new Error('Flash 读取连续收到空帧，传输无进展。');
      continue;
    }
    emptyFrames = 0;
    const expected = Math.min(packetSize, size - received);
    if (packet.length !== expected) throw new Error(`Flash 读取包长度不符：应为 ${expected}，实为 ${packet.length}。`);
    output.set(packet, received); received += packet.length;
    const ack = new Uint8Array(4); new DataView(ack.buffer).setUint32(0, received, true);
    await loader.transport.write(ack); progress(received);
  }
  const digest = await loader.transport.read(5000);
  signal.throwIfAborted();
  if (digest.length !== 16) throw new Error('Flash 读取缺少结束校验包。');
  if (hex(digest) !== hex(md5(output))) throw new Error('Flash 读取传输 MD5 不符。');
  return output;
}
export class BrowserSerialDevice implements SerialDevice {
  #transport: Transport;
  #loader: ESPLoader;
  #closing?: Promise<void>;
  #connection?: Promise<DeviceInfo>;
  #writable = false;
  constructor(port: SerialPort, private resetMode: 'default_reset' | 'usb_reset' | 'no_reset') {
    const info = port.getInfo?.() ?? {};
    if ((info.usbVendorId !== undefined && info.usbVendorId !== 0x303a) || (info.usbProductId !== undefined && info.usbProductId !== 0x1001)) {
      throw new Error('选中的不是小纸的 USB JTAG_serial debug unit，请重新选择串口。');
    }
    this.#transport = new Transport(port, false, true);
    this.#loader = new ESPLoader({ transport: this.#transport, baudrate: 115200, romBaudrate: 115200,
      serialOptions: { bufferSize: 8192 },
      debugLogging: false, enableTracing: false, terminal: { clean() {}, write() {}, writeLine() {} } });
  }
  connect(signal: AbortSignal): Promise<DeviceInfo> {
    return this.#connection ??= this.#connect(signal);
  }
  async #connect(signal: AbortSignal): Promise<DeviceInfo> {
    signal.throwIfAborted();
    // A native USB-JTAG port does not always honour DTR/RTS, so on some macOS
    // browsers the ROM never sees the reset and the sync just waits. Extra
    // attempts are free, and a hard deadline turns a silent hang into a message
    // that tells the user what to do instead.
    let timer: ReturnType<typeof setTimeout> | undefined;
    const detect = this.#loader.detectChip(this.resetMode);
    detect.catch(() => {}); // the losing side of the race must not go unhandled
    const deadline = new Promise<never>((_, reject) => {
      timer = setTimeout(() => reject(new Error('连接设备超时')), CONNECT_TIMEOUT_MS);
    });
    try {
      await Promise.race([detect, deadline]);
    } catch (error) {
      signal.throwIfAborted();
      // Never block the message on a close that may itself be waiting for the
      // pending read: report first, clean up in the background.
      void this.#transport.disconnect().catch(() => {});
      const reason = error instanceof Error ? error.message : '连接失败';
      throw new Error(`${reason}。请确认串口选的是「USB JTAG_serial debug unit」，换一根数据线或换个 USB 口后再点一次；仍不行请用 Chrome，或改用下面的手动方式。`);
    } finally {
      if (timer) clearTimeout(timer);
    }
    signal.throwIfAborted();
    const chip = this.#loader.chip.CHIP_NAME;
    if (chip !== 'ESP32-S3') throw new Error('不是 ESP32-S3，已停止。');
    const info = await this.#loader.getSecurityInfo();
    const securitySafe = !info.parsedFlags.SECURE_BOOT_EN && !info.parsedFlags.SECURE_DOWNLOAD_ENABLE && info.flashCryptCnt === 0;
    if (!securitySafe) throw new Error('安全启动、Flash 加密或安全下载状态不受支持。');
    if (this.#loader.chip.postConnect) await this.#loader.chip.postConnect(this.#loader);
    await this.#loader.runStub();
    signal.throwIfAborted();
    if (!this.#loader.IS_STUB) throw new Error('无法启动设备校验所需的 RAM stub。');
    const flashId = await this.#loader.readFlashId();
    signal.throwIfAborted();
    // SPI RDID is returned as a little-endian register integer, not printed JEDEC order.
    const capacityCode = (flashId >>> 16) & 0xff;
    this.#writable = flashId === SUPPORTED_FLASH_ID && capacityCode === 0x18 && securitySafe;
    return { chip, flashSize: capacityCode === 0x18 ? FLASH_SIZE : 0, flashId, securitySafe };
  }
  async read(offset: number, size: number, signal: AbortSignal, progress: (bytes: number) => void): Promise<Uint8Array> {
    return readStubFlash(this.#loader, offset, size, signal, progress);
  }
  async md5(offset: number, size: number, signal: AbortSignal): Promise<string> {
    signal.throwIfAborted();
    if (!this.#loader.IS_STUB || !Number.isInteger(offset) || !Number.isInteger(size) || offset < 0 || size <= 0 || offset + size > FLASH_SIZE) throw new Error('设备端 MD5 校验范围无效。');
    const digest = await this.#loader.flashMd5sum(offset, size);
    signal.throwIfAborted();
    if (typeof digest !== 'string' || !/^[0-9a-f]{32}$/i.test(digest)) throw new Error('设备端 MD5 响应无效。');
    return digest.toLowerCase();
  }
  async reset(signal: AbortSignal): Promise<void> {
    signal.throwIfAborted();
    // after('hard_reset') only writes RTS=false, which is already false after
    // USB-JTAG sync. Pulse EN low with IO0 high to leave the RAM stub and boot.
    await this.#transport.setSignals(false, true);
    await new Promise(resolve => setTimeout(resolve, 200));
    await this.#transport.setSignals(false, false);
    await new Promise(resolve => setTimeout(resolve, 200));
    signal.throwIfAborted();
  }
  async write(part: Part, bytes: Uint8Array, signal: AbortSignal, progress: (bytes: number, total: number) => void): Promise<void> {
    signal.throwIfAborted();
    const maximum = new Map([[0, 0x8000], [0x8000, 4096], [0x10000, 0x300000], [0x910000, 8192]]).get(part.offset);
    if (!this.#writable || this.#closing || !maximum || !Number.isInteger(part.size) || part.size <= 0 || part.size > maximum || bytes.length !== part.size || await sha256(bytes) !== part.sha256 ||
      (part.offset === 0x8000 && part.size % 32 !== 0) || (part.offset === 0x910000 && (part.size !== 8192 || !bytes.every(b => b === 255)))) throw new Error('串口写入门禁拒绝此地址、芯片、长度或哈希。');
    signal.throwIfAborted();
    // Keep signed bytes unchanged: no automatic flash-header patching, no erase-all,
    // no merged image, no NVS/resource/OTA-slot writes. Compression avoids large
    // write-block padding; upstream 4-byte padding remains in allowlisted erase sectors.
    await this.#loader.writeFlash({ fileArray: [{ address: part.offset, data: bytes }], flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',
      eraseAll: false, compress: true, calculateMD5Hash: data => hex(md5(data)),
      reportProgress: (_file, written, total) => { signal.throwIfAborted(); progress(written, total); } });
    signal.throwIfAborted();
  }
  close(): Promise<void> {
    this.#writable = false;
    return this.#closing ??= (async () => {
      // A pending port.open/reset must settle first: otherwise disconnect could run
      // before open finishes and leave a freshly opened port behind after cancel.
      await this.#connection?.catch(() => {});
      this.#writable = false;
      await this.#transport.disconnect();
    })();
  }
}
