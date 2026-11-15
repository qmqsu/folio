import { fetchRelease, type Release } from './manifest';
import { Installer, type Update } from './controller';
import { browserSupport, BrowserSerialDevice } from './serial';

const element = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const button = (id: string) => element<HTMLButtonElement>(id);
const checkbox = (id: string) => element<HTMLInputElement>(id);
const channel = 'stable' as const;
const reset = element<HTMLSelectElement>('reset-mode');
const status = element<HTMLParagraphElement>('install-status');
const progress = element<HTMLProgressElement>('install-progress');
const support = browserSupport();
element('browser-support').textContent = support.reason;
button('check-release').disabled = !isSecureContext || !crypto.subtle;
let release: Release | undefined;
let installer: Installer | undefined;
let operation = false;
let checking = false;
let pickerPending = false;
let checkAbort: AbortController | undefined;
let binaryDownloaded = false;
let evidenceDownloaded = false;
let backupUrl: string | undefined;
let evidenceUrl: string | undefined;
let lastProgress = 0;
let lastStage = '';

function guardActive(): boolean { return operation || pickerPending || installer?.connected === true; }
function announce(message: string): void { status.textContent = message; }
function updateWorkflowStage(): void {
  const stage = installer?.stage;
  const current = installer && (installer.stage === 'awaiting-confirmation' || operation || ['success', 'failed', 'cancelled'].includes(stage ?? '')) ? 2 : 1;
  for (const [index, item] of [...document.querySelectorAll('.workbench-steps li')].entries()) {
    item.classList.toggle('is-current', index + 1 === current);
    if (index + 1 === current) item.setAttribute('aria-current', 'step');
    else item.removeAttribute('aria-current');
  }
}
function updateControls(): void {
  updateWorkflowStage();
  const active = guardActive();
  reset.disabled = active;
  checkbox('physical-model').disabled = active;
  button('check-release').disabled = active || checking || !isSecureContext || !crypto.subtle;
  button('connect-backup').disabled = active || checking || !release || !support.supported || !checkbox('physical-model').checked;
  button('cancel').disabled = !(active || checking) || pickerPending;
  const ready = installer?.stage === 'awaiting-confirmation';
  for (const id of ['backup-saved', 'factory-confirm', 'migration-confirm']) checkbox(id).disabled = !ready || operation;
  button('install-button').disabled = operation || !ready || !installer?.compatible || !binaryDownloaded || !evidenceDownloaded ||
    !checkbox('backup-saved').checked || !checkbox('factory-confirm').checked || (installer.migration && !checkbox('migration-confirm').checked);
}
function render(update: Update): void {
  const now = performance.now();
  if (update.stage !== lastStage || update.done === undefined || update.done === update.total || now - lastProgress > 250) {
    announce(update.message);
    if (update.done !== undefined && update.total) {
      const percent = Math.min(100, Math.floor(update.done / update.total * 100));
      progress.value = percent;
      element('progress-value').textContent = `${percent}% · ${update.done.toLocaleString()} / ${update.total.toLocaleString()} B`;
    } else {
      progress.value = 0;
      element('progress-value').textContent = ['success', 'failed', 'cancelled'].includes(update.stage) ? '已结束' : '等待 / 校验中';
    }
    lastProgress = now; lastStage = update.stage;
  }
  if (['failed', 'cancelled'].includes(update.stage)) {
    element<HTMLDetailsElement>('recovery').open = true;
  }
  updateControls();
}
function clearDownloads(): void {
  if (backupUrl) URL.revokeObjectURL(backupUrl);
  if (evidenceUrl) URL.revokeObjectURL(evidenceUrl);
  backupUrl = evidenceUrl = undefined;
  binaryDownloaded = evidenceDownloaded = false;
  element('backup-panel').hidden = true;
  for (const id of ['backup-saved', 'factory-confirm', 'migration-confirm']) checkbox(id).checked = false;
}
function showBackup(): void {
  const evidence = installer?.evidence, bytes = installer?.backup;
  if (!evidence || !bytes) return;
  backupUrl = URL.createObjectURL(new Blob([new Uint8Array(bytes)], { type: 'application/octet-stream' }));
  evidenceUrl = URL.createObjectURL(new Blob([JSON.stringify(evidence, null, 2) + '\n'], { type: 'application/json' }));
  element('backup-panel').hidden = false;
  element('backup-title').focus();
  element('migration-field').hidden = !installer?.migration;
  element('backup-details').textContent = `${evidence.size.toLocaleString()} 字节 / 16MiB\nSHA-256: ${evidence.sha256}\n布局：${evidence.layout}`;
}
function download(url: string | undefined, name: string): boolean {
  if (!url) return false;
  const link = document.createElement('a');
  link.href = url; link.download = name; document.body.append(link); link.click(); link.remove();
  return true; // Dispatch only. User must still acknowledge actual files on disk.
}
button('check-release').addEventListener('click', async () => {
  if (checking || guardActive()) return;
  release = undefined; clearDownloads(); installer = undefined;
  checking = true; checkAbort = new AbortController(); updateControls();
  element('release-badge').textContent = '校验中';
  announce('获取通道清单并用固定公钥验证签名；不会连接设备。');
  try {
    const selected = channel;
    const verified = await fetchRelease(selected, checkAbort.signal);
    checkAbort.signal.throwIfAborted();
    // A local high-water mark prevents a previously observed channel rollback.
    // It is not device anti-rollback and clearing browser data resets it.
    const key = `folio-release-sequence-${selected}`;
    let previous = 0;
    try { previous = Number(localStorage.getItem(key) ?? '0'); } catch { /* optional storage */ }
    if (Number.isFinite(previous) && verified.sequence < previous) throw new Error('发布序列低于此浏览器已见版本，已阻止降级。');
    try { localStorage.setItem(key, String(verified.sequence)); } catch { /* no private data stored */ }
    release = verified;
    element('release-badge').textContent = '签名有效';
    element('release-info').textContent = `${verified.version} · ${verified.channel} · 序列 ${verified.sequence} · 四段发布者签名已验证。签名不代表硬件验收。`;
    announce('清单签名有效。核对实物型号后，可选择设备并读取备份。');
  } catch (error) {
    const message = error instanceof Error ? error.message : '检查失败';
    element('release-badge').textContent = message.includes('未发布') ? '未发布' : '不可用';
    element('release-info').textContent = `${message} 连接与烧录保持禁用。`;
    announce(checkAbort.signal.aborted ? '已取消发布检查，未连接设备。' : `${message} 安装已禁用。`);
  } finally { checking = false; updateControls(); }
});
for (const id of ['physical-model', 'backup-saved', 'factory-confirm', 'migration-confirm']) checkbox(id).addEventListener('change', updateControls);
button('connect-backup').addEventListener('click', async () => {
  if (!release || guardActive() || !support.supported || !checkbox('physical-model').checked) return;
  clearDownloads(); pickerPending = true; updateControls();
  try {
    // Must run directly inside the user gesture, BEFORE downloads / any await.
    // No getPorts(), automatic reconnection or USB VID/PID-as-model assumptions.
    const port = await navigator.serial.requestPort();
    pickerPending = false; operation = true;
    installer = new Installer(release, new BrowserSerialDevice(port, reset.value as 'default_reset' | 'usb_reset' | 'no_reset'), render);
    updateControls();
    await installer.prepare(checkbox('physical-model').checked);
    showBackup();
  } catch (error) { announce(error instanceof Error ? `未开始安装：${error.message}` : '未开始安装。'); }
  finally { pickerPending = false; operation = false; updateControls(); }
});
button('save-backup').addEventListener('click', () => {
  binaryDownloaded = download(backupUrl, `folio-original-16MiB-${installer?.evidence?.sha256.slice(0, 12)}.bin`);
  announce('已请求下载备份。请到下载目录核对实际文件；网站无法替你确认已落盘。'); updateControls();
});
button('save-evidence').addEventListener('click', () => {
  evidenceDownloaded = download(evidenceUrl, `folio-backup-sha256-${installer?.evidence?.sha256.slice(0, 12)}.json`);
  announce('已请求下载 SHA-256 证据。请与备份一起妥善保存，不要上传。'); updateControls();
});
button('install-button').addEventListener('click', async () => {
  if (button('install-button').disabled || !installer?.evidence) return;
  try {
    installer.confirmBackupSaved(installer.evidence.sha256, binaryDownloaded, evidenceDownloaded, checkbox('backup-saved').checked);
    operation = true; updateControls();
    await installer.install(checkbox('factory-confirm').checked, checkbox('migration-confirm').checked);
  } catch (error) { announce(error instanceof Error ? error.message : '安装未完成。'); }
  finally { operation = false; updateControls(); }
});
button('cancel').addEventListener('click', async () => {
  checkAbort?.abort();
  button('cancel').disabled = true;
  announce('正在取消并关闭串口；若连接正在复位，请等待当前尝试结束，不要拔线。');
  if (installer) await installer.cancel();
  updateControls();
});
window.addEventListener('beforeunload', event => {
  if (guardActive()) { event.preventDefault(); event.returnValue = ''; }
});
document.addEventListener('click', event => {
  const link = (event.target as Element).closest('a');
  if (!link || !guardActive() || link.hasAttribute('download')) return;
  const target = new URL(link.href);
  // In-page anchors do not interrupt the session; leaving the document does.
  if (target.origin === location.origin && target.pathname === location.pathname && target.hash) return;
  if (!window.confirm('USB 会话尚未结束。离开可能中断写入且无法自动恢复。建议先取消并关闭串口。仍要离开吗？')) event.preventDefault();
});
window.addEventListener('pagehide', () => { checkAbort?.abort(); if (installer?.connected) void installer.cancel(); });
updateControls();
