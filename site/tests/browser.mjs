import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, mkdir, access, writeFile, stat } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
import { chromium } from 'playwright-core';

const site = resolve(import.meta.dirname, '..');
const dist = resolve(site, 'dist');
const evidence = resolve(site, process.env.FOLIO_BROWSER_EVIDENCE || '../artifacts/firmware-0832/site');
await mkdir(evidence, { recursive: true });
const browserPath = process.env.FOLIO_TEST_BROWSER || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
await access(browserPath);

const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  if (!url.pathname.startsWith('/folio/')) { res.writeHead(404).end(); return; }
  const relative = decodeURIComponent(url.pathname.slice('/folio/'.length)) || 'index.html';
  const path = resolve(dist, relative.endsWith('/') ? relative + 'index.html' : relative);
  if (!path.startsWith(dist + sep)) { res.writeHead(403).end(); return; }
  try {
    const bytes = await readFile(path);
    const types = { '.html': 'text/html; charset=utf-8', '.css': 'text/css', '.js': 'text/javascript', '.svg': 'image/svg+xml', '.webp': 'image/webp', '.png': 'image/png' };
    res.writeHead(200, { 'content-type': types[extname(path)] || 'application/octet-stream' }); res.end(bytes);
  } catch { res.writeHead(404).end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/folio/`;
const origin = url.replace(/\/$/, '');

const report = {
  kind: 'real-headless-browser-visual-test',
  browser: browserPath,
  url,
  hardware: 'NO SERIAL ACCESS. Release fetches mocked; requestPort is a throwing sentinel, so no device is touched.',
  viewports: [],
  checks: [],
  errors: []
};

let context;
try {
  context = await chromium.launchPersistentContext(resolve(site, '.browser-test-profile'), {
    executablePath: browserPath, headless: true, viewport: { width: 1440, height: 1000 },
    reducedMotion: 'reduce', args: ['--disable-background-networking', '--no-first-run', '--disable-sync'],
  });
  await context.route('https://qmqsu.github.io/**', route => route.fulfill({ status: 404, body: 'Not released (browser test fixture)' }));
  await context.addInitScript(() => {
    window.__serialCalls = 0;
    Object.defineProperty(navigator, 'serial', {
      value: { requestPort: () => { window.__serialCalls++; throw new Error('Serial forbidden in visual tests'); } },
      configurable: true
    });
  });

  const page = await context.newPage();
  page.on('pageerror', error => report.errors.push(error.message));
  const consoleErrors = [];
  const offsite = [];
  page.on('console', message => { if (message.type() === 'error' && !message.text().includes('404')) consoleErrors.push(message.text()); });
  page.on('request', request => {
    const target = request.url();
    if (!target.startsWith(origin) && !target.startsWith('https://qmqsu.github.io/') && !target.startsWith('data:')) offsite.push(target);
  });

  for (const [width, height] of [[1440, 1000], [1024, 768], [768, 1024], [375, 812], [320, 740], [844, 390]]) {
    await page.setViewportSize({ width, height });
    await page.goto(url, { waitUntil: 'networkidle' });
    const horizontal = await page.evaluate(() => ({ scroll: document.documentElement.scrollWidth, client: document.documentElement.clientWidth }));
    assert.ok(horizontal.scroll <= horizontal.client, `${width}px horizontal overflow ${JSON.stringify(horizontal)}`);
    assert.equal(await page.locator('h1').count(), 1, 'exactly one h1');
    assert.equal(await page.locator('main > section').count(), 5, 'hero + flash + interfaces + features + about');
    for (const id of ['flash', 'interfaces', 'features', 'about']) assert.equal(await page.locator(`#${id}`).count(), 1, `#${id} exists`);
    const broken = await page.evaluate(() => [...document.images].filter(i => !i.complete || i.naturalWidth === 0).map(i => i.currentSrc || i.src));
    assert.deepEqual(broken, [], `${width}px broken images`);
    // The flasher must never be armed without a verified release (mock returns 404 here).
    await page.waitForFunction(() => !document.querySelector('#flash-button')?.disabled === false || document.querySelector('#flash-status')?.textContent?.length > 4);
    assert.equal(await page.locator('#flash-button').isDisabled(), true, `${width}px: flasher disabled without release`);
    assert.equal(await page.locator('#flash-backup').count(), 0, `${width}px: no bulk-backup control`);
    assert.equal(await page.locator('#flash-save').count(), 0, `${width}px: no backup download link`);
    assert.equal(await page.locator('#flash-progress').isHidden(), true);
    // The retired full installer must not come back by accident.
    const leftovers = await page.evaluate(() => ['connect-backup', 'check-release', 'install-button', 'browser-support', 'backup-panel', 'release-badge']
      .filter(id => document.getElementById(id)));
    assert.deepEqual(leftovers, [], 'old installer DOM must be gone');
    report.viewports.push({ width, height, overflow: false });
    if (width === 1440 || width === 375) {
      await page.screenshot({ path: resolve(evidence, `home-${width}-full.png`), fullPage: true });
      await page.screenshot({ path: resolve(evidence, `home-${width}-viewport.png`) });
    }
  }

  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto(url, { waitUntil: 'networkidle' });
  // This context keeps every init script; only the new page sees the version line
  // without Web Serial, so assert on a fresh page instead of reloading this one.
  // A Firefox user agent fails the Chrome/Edge gate, which is what a phone or
  // Safari visitor hits; the version line must explain it instead of the placeholder.
  const unsupported = await context.browser().newContext({
    viewport: { width: 1440, height: 1000 },
    userAgent: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:128.0) Gecko/20100101 Firefox/128.0',
  });
  await unsupported.route('https://qmqsu.github.io/**', route => route.fulfill({ status: 404, body: 'Not released (browser test fixture)' }));
  const plain = await unsupported.newPage();
  await plain.goto(url, { waitUntil: 'networkidle' });
  await plain.waitForTimeout(300);
  assert.match(await plain.locator('#flash-version').innerText(), /需要桌面版 Chrome 或 Edge/i);
  assert.match(await plain.locator('#flash-status').innerText(), /Web Serial/);
  assert.equal(await plain.locator('#flash-button').isDisabled(), true);
  report.checks.push('No Web Serial: version line explains the browser requirement');
  await unsupported.close();
  await page.goto(url, { waitUntil: 'networkidle' });
  await page.waitForTimeout(300);
  assert.match(await page.locator('#flash-version').innerText(), /还没发布|暂时拿不到/);
  assert.match(await page.locator('#flash-status').innerText(), /还没有发布|拿不到/);
  assert.equal(await page.evaluate(() => window.__serialCalls), 0);
  report.checks.push('Mock 404: flasher stays disabled, no serial picker');

  await page.keyboard.press('Tab');
  assert.equal(await page.locator('.skip-link').evaluate(el => el === document.activeElement), true);
  assert.notEqual(await page.locator('.skip-link').evaluate(el => getComputedStyle(el).outlineStyle), 'none');
  report.checks.push('Keyboard skip link and visible focus');

  await page.locator('a.nav-cta').click();
  await page.waitForTimeout(150);
  assert.equal(new URL(page.url()).hash, '#flash');
  report.checks.push('Hero and nav both land on the flasher');

  await context.unroute('https://qmqsu.github.io/**');
  await context.route('https://qmqsu.github.io/**', route => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ key_id: 'attacker', payload: 'e30=', signature: 'AAAA' }) }));
  await page.goto(url, { waitUntil: 'networkidle' });
  await page.waitForTimeout(400);
  assert.match(await page.locator('#flash-status').innerText(), /密钥|签名/);
  assert.equal(await page.locator('#flash-button').isDisabled(), true);
  assert.equal(await page.evaluate(() => window.__serialCalls), 0);
  report.checks.push('Mock untrusted envelope rejected; flasher disabled; no serial access');

  await page.locator('a[href="/folio/migrate/"]').click();
  await page.locator('#check-release').click();
  await page.waitForFunction(() => document.querySelector('#release-badge')?.textContent === '不可用');
  assert.equal(await page.locator('#connect-backup').isDisabled(), true);
  assert.equal(await page.locator('#install-button').isDisabled(), true);
  for (const id of ['physical-model', 'backup-saved', 'factory-confirm', 'migration-confirm']) assert.equal(await page.locator('#' + id).isChecked(), false);
  assert.equal(await page.evaluate(() => window.__serialCalls), 0);
  report.checks.push('Migration link opens isolated backup wizard; unsigned manifest and unchecked consent cannot open serial or write');
  await page.goto(url, { waitUntil: 'networkidle' });

  const withoutJs = await context.browser().newContext({ javaScriptEnabled: false, viewport: { width: 375, height: 812 } });
  const fallback = await withoutJs.newPage();
  await fallback.goto(url, { waitUntil: 'networkidle' });
  assert.equal(await fallback.locator('h1').count(), 1);
  assert.match(await fallback.locator('.hero-subtitle').innerText(), /轻量化阅读固件/);
  assert.equal(await fallback.locator('#features article').count(), 6);
  assert.equal(await fallback.locator('#flash-button').isDisabled(), true);
  report.checks.push('No JavaScript: content readable, flasher inert');
  await withoutJs.close();

  const resources = await page.evaluate(() => performance.getEntriesByType('resource').map(entry => entry.name));
  const foreign = resources.filter(name => !name.startsWith(origin) && !name.startsWith('https://qmqsu.github.io/'));
  assert.deepEqual(foreign, [], 'page must only reach its own origin and the manifest host');
  assert.deepEqual(offsite, []);
  report.checks.push('No CDN, no remote font: only self-hosted assets and the manifest host');

  const scripts = await page.evaluate(() => [...document.querySelectorAll('script')].map(s => s.getAttribute('src')));
  assert.equal(scripts.length, 1, 'exactly one bundled flasher script');
  assert.ok(scripts[0]?.startsWith('/folio/_astro/'), 'script is self-hosted');
  report.checks.push('One self-hosted module: the flasher UI');

  report.errors.push(...consoleErrors);
  assert.deepEqual(report.errors, []);
  report.completed = true;
  report.indexBytes = (await stat(resolve(dist, 'index.html'))).size;
} catch (error) {
  report.completed = false;
  report.failure = error.stack;
  throw error;
} finally {
  if (context) await context.close();
  await new Promise(resolve => server.close(resolve));
  await writeFile(resolve(evidence, 'browser-report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report, null, 2));
}
