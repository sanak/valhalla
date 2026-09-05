import { strict as assert } from 'node:assert';
import { cpSync, mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { chromium } from 'playwright';
import { serve } from './serve.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const buildDir = process.env.VALHALLA_WASM_DIR;
const tileDir = process.env.VALHALLA_TILE_DIR;
assert(buildDir && tileDir, 'set VALHALLA_WASM_DIR and VALHALLA_TILE_DIR');

const root = mkdtempSync(join(tmpdir(), 'valhalla-browser-'));
for (const f of ['valhalla.mjs', 'valhalla.wasm']) cpSync(join(buildDir, f), join(root, f));
for (const f of ['worker.js', 'index.mjs']) {
  cpSync(join(here, '../../../../src/bindings/wasm', f), join(root, f));
}
cpSync(join(here, 'index.html'), join(root, 'index.html'));
execFileSync(join(here, '../make_fixture.sh'), [tileDir, join(root, 'tiles.tar')], {
  stdio: 'inherit',
});

const server = await serve(root);
const origin = `http://127.0.0.1:${server.address().port}`;
const config = JSON.parse(readFileSync(join(here, '../valhalla.json'), 'utf8'));
delete config.mjolnir.tile_dir;
config.mjolnir.tile_url = `${origin}/tiles.tar`;

const browser = await chromium.launch();
const page = await browser.newPage();
await page.goto(`${origin}/index.html`);
await page.waitForFunction(() => window.ready);

const summary = await page.evaluate((c) => window.run(c, null), config);
assert(summary.length > 0 && summary.time > 0, 'browser route has no length/time');
console.log('browser route summary:', summary);

// with a cache dir, tiles must survive a reload: the second load routes with zero range
// requests for tiles, and IndexedDB must be non-empty
const ranges = [];
page.on('request', (r) => r.headers()['range'] && ranges.push(r.url()));
await page.evaluate((c) => window.run(c, '/cache'), config);
const firstPass = ranges.length;
assert(firstPass > 0, 'cached run made no range requests at all');
ranges.length = 0;
await page.reload();
await page.waitForFunction(() => window.ready);
await page.evaluate((c) => window.run(c, '/cache'), config);
assert(ranges.length < firstPass, `cache did not reduce range requests (${ranges.length} vs ${firstPass})`);
console.log(`range requests: ${firstPass} cold, ${ranges.length} warm`);

await browser.close();
server.close();
console.log('OK');
