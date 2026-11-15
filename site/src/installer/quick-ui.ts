import { fetchRelease, type Release } from './manifest';
import { QuickFlash, type QuickUpdate } from './quick';
import { browserSupport, BrowserSerialDevice } from './serial';

const button = document.getElementById('flash-button') as HTMLButtonElement | null;
const status = document.getElementById('flash-status') as HTMLParagraphElement | null;
const version = document.getElementById('flash-version') as HTMLElement | null;
const progress = document.getElementById('flash-progress') as HTMLProgressElement | null;
const backupBox = document.getElementById('flash-backup') as HTMLInputElement | null;
const saveLink = document.getElementById('flash-save') as HTMLAnchorElement | null;

if (button && status) {
  const support = browserSupport();
  let release: Release | undefined;
  let busy = false;
  let flash: QuickFlash | undefined;

  const say = (text: string): void => { status.textContent = text; };
  const sync = (): void => {
    button.disabled = busy || !release || !support.supported;
    if (backupBox) { backupBox.checked = true; backupBox.disabled = true; }
  };
  let backupUrl: string | undefined;

  const keepBackup = ({ bytes, sha256 }: { bytes: Uint8Array; sha256: string }): void => {
    if (backupUrl) URL.revokeObjectURL(backupUrl);
    backupUrl = URL.createObjectURL(new Blob([new Uint8Array(bytes)], { type: 'application/octet-stream' }));
    const name = `folio-backup-${sha256.slice(0, 8)}.bin`;
    if (saveLink) { saveLink.href = backupUrl; saveLink.download = name; saveLink.hidden = false; saveLink.textContent = `保存备份（${name}）`; }
    const link = document.createElement('a');
    link.href = backupUrl; link.download = name;
    document.body.append(link); link.click(); link.remove();
  };

  const showProgress = (update: QuickUpdate): void => {
    if (!progress) return;
    if (update.total) {
      progress.hidden = false;
      progress.value = Math.min(100, Math.floor((update.done ?? 0) / update.total * 100));
    } else if (update.stage === 'success' || update.stage === 'failed' || update.stage === 'cancelled') {
      progress.hidden = true;
    }
  };

  if (!support.supported) {
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
      const port = await navigator.serial.requestPort();
      const device = new BrowserSerialDevice(port, 'default_reset');
      flash = new QuickFlash(release, device, update => { say(update.message); showProgress(update); });
      await flash.run({ backup: backupBox?.checked === true, onBackup: keepBackup });
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
