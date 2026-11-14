import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, mkdir, access, writeFile } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
import { chromium } from 'playwright-core';

const site = resolve(import.meta.dirname, '..');
const dist = resolve(site, 'dist');
const evidence = resolve(site, '../artifacts/firmware-0832/site');
await mkdir(evidence, { recursive: true });
const browserPath = process.env.FOLIO_TEST_BROWSER || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
await access(browserPath);
const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  if (!url.pathname.startsWith('/folio/')) { res.writeHead(404).end(); return; }
  const relative = decodeURIComponent(url.pathname.slice('/folio/'.length)) || 'index.html';
  const path = resolve(dist, relative);
  if (!path.startsWith(dist + sep)) { res.writeHead(403).end(); return; }
  try {
    const bytes = await readFile(path);
    const types = { '.html': 'text/html; charset=utf-8', '.css': 'text/css', '.js': 'text/javascript', '.svg': 'image/svg+xml' };
    res.writeHead(200, { 'content-type': types[extname(path)] || 'application/octet-stream' }); res.end(bytes);
  } catch { res.writeHead(404).end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/folio/`;
let context;
const report = { kind: 'real-headless-browser-visual-test', browser: browserPath, url, hardware: 'NO SERIAL ACCESS. Public release fetches mocked; no signing key or USB device used.', viewports: [], checks: [], errors: [] };
try {
  context = await chromium.launchPersistentContext(resolve(site, '.browser-test-profile'), {
    executablePath: browserPath, headless: true, viewport: { width: 1440, height: 1000 },
    reducedMotion: 'reduce', args: ['--disable-background-networking', '--no-first-run', '--disable-sync'],
  });
  await context.route('https://qmqsu.github.io/**', route => route.fulfill({ status: 404, body: 'Not released (browser test fixture)' }));
  await context.addInitScript(() => {
    window.__serialCalls = 0;
    Object.defineProperty(navigator, 'serial', { value: { requestPort: () => { window.__serialCalls++; throw new Error('Serial forbidden in visual tests'); } }, configurable: true });
  });
  const page = await context.newPage();
  page.on('pageerror', error => report.errors.push(error.message));
  const consoleErrors = [];
  page.on('console', message => { if (message.type() === 'error' && !message.text().includes('404')) consoleErrors.push(message.text()); });
  for (const [width, height] of [[1440, 1000], [1024, 768], [768, 1024], [375, 812], [320, 740], [844, 390]]) {
    await page.setViewportSize({ width, height }); await page.goto(url, { waitUntil: 'networkidle' });
    const horizontal = await page.evaluate(() => ({ scroll: document.documentElement.scrollWidth, client: document.documentElement.clientWidth }));
    assert.ok(horizontal.scroll <= horizontal.client, `${width}px horizontal overflow ${JSON.stringify(horizontal)}`);
    assert.equal(await page.locator('h1').count(), 1);
    for (const id of ['physical-model', 'backup-saved', 'factory-confirm', 'migration-confirm']) assert.equal(await page.locator(`#${id}`).isChecked(), false, `${id} must never be pre-authorized`);
    assert.equal(await page.locator('#connect-backup').isDisabled(), true); assert.equal(await page.locator('#install-button').isDisabled(), true);
    assert.equal(await page.evaluate(() => window.__serialCalls), 0);
    report.viewports.push({ width, height, overflow: false });
    if (width === 1440 || width === 375) {
      await page.screenshot({ path: resolve(evidence, `home-${width}-full.png`), fullPage: true });
      await page.screenshot({ path: resolve(evidence, `home-${width}-viewport.png`) });
    }
  }
  await page.setViewportSize({ width: 1440, height: 1000 }); await page.goto(url, { waitUntil: 'networkidle' });
  await page.keyboard.press('Tab');
  assert.equal(await page.locator('.skip-link').evaluate(el => el === document.activeElement), true);
  const focus = await page.locator('.skip-link').evaluate(el => getComputedStyle(el).outlineStyle);
  assert.notEqual(focus, 'none');
  await page.keyboard.press('Enter'); assert.equal(new URL(page.url()).hash, '#main');
  report.checks.push('Keyboard skip link and visible focus; native navigation/details/buttons');
  await page.locator('#physical-model').check();
  assert.equal(await page.locator('#connect-backup').isDisabled(), true);
  await page.locator('#check-release').click();
  await page.waitForFunction(() => document.querySelector('#release-badge').textContent === '未发布');
  assert.match(await page.locator('#install-status').innerText(), /未发布/);
  assert.equal(await page.locator('#connect-backup').isDisabled(), true); assert.equal(await page.locator('#install-button').isDisabled(), true);
  assert.equal(await page.evaluate(() => window.__serialCalls), 0);
  await page.locator('#install').screenshot({ path: resolve(evidence, 'installer-unpublished-desktop.png') });
  report.checks.push('Mock 404: unpublished, no usable release, no serial picker, no writes');
  await context.unroute('https://qmqsu.github.io/**');
  await context.route('https://qmqsu.github.io/**', route => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ key_id: 'attacker', payload: 'e30=', signature: 'AAAA' }) }));
  await page.locator('#check-release').click(); await page.waitForFunction(() => document.querySelector('#release-badge').textContent === '不可用');
  assert.match(await page.locator('#install-status').innerText(), /密钥/); assert.equal(await page.locator('#install-button').isDisabled(), true);
  report.checks.push('Mock untrusted envelope rejected; no serial access');
  await page.evaluate(() => { Object.defineProperty(navigator, 'userAgent', { configurable: true, value: 'Mozilla/5.0 iPhone Mobile Safari/605' }); });
  await page.goto(url, { waitUntil: 'networkidle' }); // Browser-support test in separate fresh mocked context below.
  const noSerial = await context.newPage();
  await noSerial.addInitScript(() => Object.defineProperty(navigator, 'userAgent', { value: 'Mozilla/5.0 iPhone Mobile Safari/605', configurable: true }));
  await noSerial.goto(url, { waitUntil: 'networkidle' });
  assert.match(await noSerial.locator('#browser-support').innerText(), /需要桌面/);
  assert.equal(await noSerial.locator('#connect-backup').isDisabled(), true);
  report.checks.push('Mobile/Safari capability mock clearly disabled'); await noSerial.close();
  const noJS = await context.browser().newContext({ javaScriptEnabled: false, viewport: { width: 375, height: 812 } });
  const fallback = await noJS.newPage(); await fallback.goto(url);
  assert.match(await fallback.locator('noscript').innerText(), /JavaScript 未启用/);
  assert.equal(await fallback.locator('#install-button').isDisabled(), true); assert.equal(await fallback.locator('h1').count(), 1);
  report.checks.push('No JavaScript: content and recovery readable, installer disabled'); await noJS.close();
  report.errors.push(...consoleErrors);
  assert.deepEqual(report.errors, []);
  report.completed = true;
} catch (error) { report.completed = false; report.failure = error.stack; throw error; }
finally {
  if (context) await context.close();
  await new Promise(resolve => server.close(resolve));
  await writeFile(resolve(evidence, 'browser-report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report, null, 2));
}
