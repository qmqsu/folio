import { readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import { dirname, resolve } from 'node:path';

const here = resolve(import.meta.dirname, '..');
const require = createRequire(import.meta.url);
const espDir = dirname(require.resolve('esptool-js/package.json'));
const espRequire = createRequire(resolve(espDir, 'package.json'));
const stub = await readFile(resolve(espDir, 'lib/targets/stub_flasher/esp32s3.json'));
if (createHash('sha256').update(stub).digest('hex') !== '1e9e67c529837f12055a9ea87545919502c6db389b9a671f0f0340620028fea3') throw new Error('Pinned ESP32-S3 RAM stub changed: review source provenance and licenses before building.');
const nobleDir = dirname(require.resolve('@noble/hashes/legacy.js'));
const pakoDir = dirname(espRequire.resolve('pako/package.json'));
const atobDir = dirname(espRequire.resolve('atob-lite/package.json'));
const specs = [
  ['esptool-js 0.7.0 — Apache-2.0 — https://github.com/espressif/esptool-js/tree/4d8e8fe092f3425a4b85d1e883f4cc078a5efdd3', resolve(espDir, 'LICENSE')],
  ['RAM flasher stub provenance and pinned source versions', resolve(here, 'public/licenses/stub-provenance.txt')],
  ['esp-flasher-stub v1.2.2 + esp-stub-lib — MIT selection', resolve(here, 'public/licenses/esp-flasher-stub-MIT.txt')],
  ['@noble/hashes 2.0.1 — MIT — https://github.com/paulmillr/noble-hashes', resolve(nobleDir, 'LICENSE')],
  ['pako 2.2.0 — MIT and zlib — https://github.com/nodeca/pako', resolve(pakoDir, 'LICENSE')],
  ['atob-lite 2.0.0 — MIT — https://github.com/hughsk/atob-lite', resolve(atobDir, 'LICENSE.md')],
];
const texts = await Promise.all(specs.map(async ([title, path]) => `${title}\n${'='.repeat(72)}\n${await readFile(path, 'utf8')}`));
const zlibHeader = (await readFile(resolve(pakoDir, 'lib/zlib/deflate.js'), 'utf8')).split('\n').slice(2, 20).map(line => line.replace(/^\/\/ ?/, '')).join('\n');
const text = `Folio website browser-bundle third-party notices\n\nThe original Folio website code is MIT licensed. Dependencies retain their own licenses.\nNo third-party source files are modified; they are bundled/minified for static delivery.\nThe READ_FLASH framing adapter is an independent implementation of the documented esptool protocol, not a copy of Python source.\n\n${texts.join('\n\n')}\n\nPako's zlib-port notice\n${'='.repeat(72)}\n${zlibHeader}\n`;
await writeFile(resolve(here, 'public/third-party-licenses.txt'), text);
console.log('Wrote browser-bundle license notices.');
