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

// a worker that cannot load must reject create(), not leave it pending forever
let timer;
const badWorker = await Promise.race([
  page.evaluate(() => window.badWorker()),
  new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error('create() hung on an unloadable worker')), 15000);
  }),
]);
clearTimeout(timer);
assert.equal(badWorker, 'ValhallaError', `unloadable worker returned ${badWorker}`);
console.log('an unloadable worker rejects instead of hanging');

const queuedAbort = await page.evaluate((c) => window.abortQueued(c), config);
assert.equal(queuedAbort.name, 'AbortError', `queued abort returned ${queuedAbort.name}`);
assert.equal(queuedAbort.firstOk, true, 'aborting a queued request killed the running one');
console.log('aborting a queued request leaves the running one alone');

// counts range requests made only after the 'valhalla-followup-start' marker, i.e. during the
// follow-up route() call inside abortInflight - not the aborted isochrone that precedes it
async function runAbortInflight(pg, cfg) {
  const followupRanges = [];
  let capturing = false;
  const onRequest = (r) => {
    if (capturing && r.headers()['range']) followupRanges.push(r.url());
  };
  const onConsole = (msg) => {
    if (msg.text() === 'valhalla-followup-start') capturing = true;
  };
  pg.on('request', onRequest);
  pg.on('console', onConsole);
  const result = await pg.evaluate((c) => window.abortInflight(c), cfg);
  pg.off('request', onRequest);
  pg.off('console', onConsole);
  return { ...result, followupRanges: followupRanges.length };
}

const inflight = await runAbortInflight(page, config);
assert.equal(inflight.name, 'AbortError', `in-flight abort returned ${inflight.name}`);
assert.equal(inflight.after, true, 'the instance was unusable after an in-flight abort');
// the respawn settles the abort within a macrotask; without it the entry only settles when the
// worker's own result arrives, which costs the whole request - measured at 0.3ms against 30ms
assert(inflight.ms < 8, `in-flight abort took ${inflight.ms}ms - the worker result won`);
console.log(`in-flight abort works in ${inflight.ms.toFixed(1)}ms` +
  ` (crossOriginIsolated=${inflight.isolated}, follow-up range requests=${inflight.followupRanges})`);

// a cross-origin-isolated page has SharedArrayBuffer, so the abort takes the cooperative path:
// the worker is interrupted in place instead of being torn down and respawned
const isolatedServer = await serve(root, 0, { crossOriginIsolated: true });
const isolatedOrigin = `http://127.0.0.1:${isolatedServer.address().port}`;
const isolatedConfig = structuredClone(config);
// COEP blocks a cross-origin tar fetch, so the isolated page must pull tiles from its own origin
isolatedConfig.mjolnir.tile_url = `${isolatedOrigin}/tiles.tar`;

const isolatedPage = await browser.newPage();
await isolatedPage.goto(`${isolatedOrigin}/index.html`);
await isolatedPage.waitForFunction(() => window.ready);

const isolated = await runAbortInflight(isolatedPage, isolatedConfig);
assert.equal(isolated.isolated, true, 'the isolated origin was not cross-origin isolated');
assert.equal(isolated.name, 'AbortError', `isolated abort returned ${isolated.name}`);
assert.equal(isolated.after, true, 'the instance was unusable after a cooperative abort');
// after==true holds for a respawned worker too, so it can't tell the mechanisms apart; a
// surviving worker still has the tiles the aborted isochrone fetched, a respawned one does not -
// measured at 4 range requests against 9 on this fixture
assert(isolated.followupRanges < inflight.followupRanges,
  `cooperative worker did not avoid re-fetching tiles (${isolated.followupRanges} isolated vs ` +
  `${inflight.followupRanges} respawned)`);
console.log('cooperative cancellation keeps the worker alive' +
  ` (follow-up range requests: ${isolated.followupRanges} isolated vs ${inflight.followupRanges} respawned)`);

isolatedServer.close();
await browser.close();
server.close();
console.log('OK');
