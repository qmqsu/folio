import { fetchRelease, type Release } from './manifest';
import { QuickFlash, type QuickUpdate } from './quick';
import { browserSupport, BrowserSerialDevice } from './serial';

const button = document.getElementById('flash-button') as HTMLButtonElement | null;
const status = document.getElementById('flash-status') as HTMLParagraphElement | null;
const version = document.getElementById('flash-version') as HTMLElement | null;
const progress = document.getElementById('flash-progress') as HTMLProgressElement | null;

if (button && status) {
  const support = browserSupport();
  let release: Release | undefined;
  let busy = false;
  let flash: QuickFlash | undefined;

  const say = (text: string): void => { status.textContent = text; };
  const sync = (): void => { button.disabled = busy || !!flash?.connected || !release || !support.supported; };
  const showProgress = (update: QuickUpdate): void => {
    if (!progress) return;
    if (update.total) {
      progress.hidden = false;
      progress.value = Math.min(100, Math.floor((update.done ?? 0) / update.total * 100));
    } else {
      progress.hidden = true;
      progress.value = 0;
    }
  };

  if (!support.supported) {
    // Without this the version line keeps its HTML placeholder and looks stuck.
    if (version) version.textContent = '需要桌面版 Chrome 或 Edge';
    say(support.reason);
    sync();
  } else {
    say('正在查最新固件…');
    sync();
    fetchRelease('stable')
      .then(value => {
        release = value;
        if (version) version.textContent = `${value.version} · ${value.channel}`;
        say('准备好了。插上数据线，点下面的按钮，选一下串口。');
        sync();
      })
      .catch((error: unknown) => {
        const message = error instanceof Error ? error.message : '检查失败';
        const unpublished = /未发布|HTTP 404|HTTP 503/.test(message);
        if (version) version.textContent = unpublished ? '还没发布' : '暂时拿不到';
        say(unpublished
          ? '这个通道还没有发布固件。发布以后，这个按钮就能用。'
          : `拿不到发布清单（${message}）可以先用下面的手动方式。`);
        sync();
      });
  }

  button.addEventListener('click', async () => {
    if (!release || busy || !support.supported) return;
    busy = true;
    sync();
    try {
      // Must stay inside the click gesture: pick the port before any await.
      const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a, usbProductId: 0x1001 }] });
      const device = new BrowserSerialDevice(port, 'default_reset');
      flash = new QuickFlash(release, device, update => { say(update.message); showProgress(update); });
      await flash.run(async bytes => {
        say('切换系统会覆盖原固件，原系统的字体等内部资源也可能被覆盖。请确认机身型号为 RDP-G01-W，保存完整备份后继续。');
        const save = document.createElement('button');
        const cancel = document.createElement('button');
        save.textContent = '确认机型并保存备份'; cancel.textContent = '取消';
        status.after(save, cancel);
        save.focus();
        try {
          return await new Promise<boolean>((resolve, reject) => {
            cancel.onclick = () => resolve(false);
            save.onclick = async () => {
              save.disabled = true; cancel.disabled = true;
              try {
                const picker = window as Window & { showSaveFilePicker?(options: { suggestedName: string }): Promise<FileSystemFileHandle> };
                if (!picker.showSaveFilePicker) throw new Error("当前浏览器无法保存并校验备份，请用桌面 Chrome 或 Edge。");
                const file = await picker.showSaveFilePicker({ suggestedName: 'folio-original-16MiB.bin' });
                const stream = await file.createWritable();
                await stream.write(new Uint8Array(bytes)); await stream.close();
                const stored = new Uint8Array(await (await file.getFile()).arrayBuffer());
                if (stored.length !== bytes.length || !bytes.every((byte,i) => byte === stored[i])) throw new Error('备份文件回读不一致。');
                resolve(window.confirm('完整备份已保存。现在替换原系统并安装 Folio？'));
              } catch (error) { reject(error); }
            };
          });
        } finally { save.remove(); cancel.remove(); }
      }, () => new BrowserSerialDevice(port, 'usb_reset'));
    } catch (error) {
      say(error instanceof Error ? `没开始：${error.message}` : '没开始。');
    } finally {
      busy = false;
      if (progress) progress.hidden = true;
      sync();
    }
  });

  window.addEventListener('beforeunload', event => {
    if (flash?.connected) { event.preventDefault(); event.returnValue = ''; }
  });
  window.addEventListener('pagehide', () => { if (flash?.connected) void flash.cancel(); });
}
