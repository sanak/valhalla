// Smoke test for the wasm bindings under node.
//   VALHALLA_WASM=<build>/src/bindings/wasm/valhalla.mjs \
//   VALHALLA_TILE_DIR=<dir with the .gph tree> node tests.mjs
import { strict as assert } from 'node:assert';
import { openSync, readSync, closeSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import process from 'node:process';
import { execFileSync } from 'node:child_process';

const modulePath = process.env.VALHALLA_WASM;
const tileDir = process.env.VALHALLA_TILE_DIR;
assert(modulePath && tileDir, 'set VALHALLA_WASM and VALHALLA_TILE_DIR');

const createValhalla = (await import(modulePath)).default;
const Module = await createValhalla();

// NODEFS maps a host directory straight into the wasm FS, so tiles are read on demand
// instead of being copied into the heap. mjolnir.tile_dir in valhalla.json is the mount point.
Module.FS.mkdir('/tiles');
Module.FS.mount(Module.NODEFS, { root: tileDir }, '/tiles');

const config = readFileSync(join(dirname(fileURLToPath(import.meta.url)), 'valhalla.json'), 'utf8');

console.log('valhalla version:', Module.version());

const actor = new Module.Actor(config);

const status = JSON.parse(actor.status('{"verbose":true}'));
assert(status.has_tiles, 'no tiles loaded');
console.log('tileset bbox:', JSON.stringify(status.bbox.features[0].geometry.coordinates[0]));

// Vaduz -> Schaan
const route = JSON.parse(
  actor.route(
    JSON.stringify({
      locations: [
        { lat: 47.141, lon: 9.521 },
        { lat: 47.165, lon: 9.51 },
      ],
      costing: 'auto',
    }),
  ),
);
const summary = route.trip.summary;
console.log('route summary:', summary);
assert(summary.length > 0 && summary.time > 0, 'route has no length/time');
console.log('maneuvers:', route.trip.legs[0].maneuvers.map((m) => m.instruction));

// valhalla errors reach JS as a ValhallaError carrying the exception's code/httpCode
assert.throws(
  () => actor.route(JSON.stringify({ locations: [{ lat: 0, lon: 0 }, { lat: 0.1, lon: 0.1 }], costing: 'auto' })),
  (e) => e.name === 'ValhallaError' && e.code === 171 && e.httpCode === 400,
  'expected ValhallaError 171 for a route with no nearby edges',
);

actor.delete();

// --- cancellation ------------------------------------------------------------------------
// Atomics on a plain SharedArrayBuffer, which node has unconditionally. The hook stands in
// for the main thread flipping the flag while the worker is blocked in the wasm call.
{
  const flag = new Int32Array(new SharedArrayBuffer(4));
  Module.cancelFlag = flag;
  Module.currentRequestId = 7;
  const cancellingActor = new Module.Actor(config);
  Atomics.store(flag, 0, 7);
  assert.throws(
    () =>
      cancellingActor.isochrone(
        JSON.stringify({
          locations: [{ lat: 47.141, lon: 9.521 }],
          costing: 'auto',
          contours: [{ time: 15 }],
        }),
      ),
    (e) => e.name === 'AbortError',
    'expected an AbortError once the cancel flag matches the running request id',
  );

  // a flag that names a different request must not touch this one
  Atomics.store(flag, 0, 8);
  const iso = cancellingActor.isochrone(
    JSON.stringify({
      locations: [{ lat: 47.141, lon: 9.521 }],
      costing: 'auto',
      contours: [{ time: 1 }],
    }),
  );
  assert(JSON.parse(iso).features.length > 0, 'a non-matching cancel id must not abort');

  cancellingActor.delete();
  Module.cancelFlag = undefined;
  Module.currentRequestId = undefined;
  console.log('cancellation aborts only the request whose id matches the flag');
}

// --- remote tar over the tileFetch hook ---------------------------------------------------
// Same tiles, reached by range requests instead of NODEFS. The hook stands in for sync XHR.
const here = dirname(fileURLToPath(import.meta.url));
const tarPath = join(process.env.TMPDIR ?? '/tmp', 'valhalla-wasm-fixture.tar');
execFileSync(join(here, 'make_fixture.sh'), [tileDir, tarPath], { stdio: 'inherit' });

const TAR_URL = 'https://example.invalid/tiles.tar';
const fd = openSync(tarPath, 'r');
let fetchCount = 0;

Module.tileFetch = (url, offset, size) => {
  if (url !== TAR_URL) return { httpCode: 404, body: null };
  fetchCount++;
  const buf = Buffer.alloc(size > 0 ? size : 1 << 24);
  const read = readSync(fd, buf, 0, buf.length, offset);
  return {
    httpCode: size > 0 ? 206 : 200,
    body: new Uint8Array(buf.buffer, buf.byteOffset, read),
  };
};

const tarConfig = JSON.parse(config);
delete tarConfig.mjolnir.tile_dir;
tarConfig.mjolnir.tile_url = TAR_URL;

const tarActor = new Module.Actor(JSON.stringify(tarConfig));
const tarRoute = JSON.parse(
  tarActor.route(
    JSON.stringify({
      locations: [
        { lat: 47.141, lon: 9.521 },
        { lat: 47.165, lon: 9.51 },
      ],
      costing: 'auto',
    }),
  ),
);
assert(fetchCount > 0, 'tileFetch was never called');
assert.deepEqual(tarRoute.trip.summary, summary, 'tar route differs from the NODEFS route');
console.log(`tar route matches, ${fetchCount} range requests`);
tarActor.delete();

// A tile the index promised must never silently vanish: index.bin listed it, so a miss means
// the tar is broken, not that the area is unroutable.
// the reader reads the tar header and index.bin while the Actor is constructed, so swapping the
// hook afterwards leaves only tile requests to fail
const brokenActor = new Module.Actor(JSON.stringify(tarConfig));
Module.tileFetch = () => ({ httpCode: 404, body: null });
assert.throws(
  () =>
    brokenActor.route(
      JSON.stringify({
        locations: [
          { lat: 47.141, lon: 9.521 },
          { lat: 47.165, lon: 9.51 },
        ],
        costing: 'auto',
      }),
    ),
  (e) => e.name === 'ValhallaError' && /HTTP status 404/.test(e.message),
  'a 404 on an indexed tile must reject, not return a detour',
);

// every action that correlates locations has to surface the tile failure, not loki's 171
const LOCATIONS = [
  { lat: 47.141, lon: 9.521 },
  { lat: 47.165, lon: 9.51 },
];
const brokenRequests = {
  isochrone: { locations: [LOCATIONS[0]], costing: 'auto', contours: [{ time: 5 }] },
  matrix: { sources: [LOCATIONS[0]], targets: [LOCATIONS[1]], costing: 'auto' },
};
for (const [action, request] of Object.entries(brokenRequests)) {
  assert.throws(
    () => brokenActor[action](JSON.stringify(request)),
    (e) => e.name === 'ValhallaError' && /HTTP status 404/.test(e.message),
    `${action} swallowed the tile failure instead of reporting it`,
  );
}
brokenActor.delete();
console.log('404 on an indexed tile fails loudly');

closeSync(fd);

// --- per-tile URLs over the same hook ------------------------------------------------------
// A `{tilePath}` URL has no listing, so an index.bin next to the tiles is what lets the reader
// enumerate the tileset and loki keep its connectivity map.
const TILE_URL_PREFIX = 'https://example.invalid/tiles/';
const tileRequests = [];

// the fixture tar already carries the index valhalla_build_extract produced, as its first member
const indexBin = (() => {
  const tarFd = openSync(tarPath, 'r');
  const header = Buffer.alloc(512);
  readSync(tarFd, header, 0, 512, 0);
  const name = header.toString('ascii', 0, 100).replace(/\0.*$/, '');
  assert.equal(name, 'index.bin', 'the fixture tar does not start with index.bin');
  const size = parseInt(header.toString('ascii', 124, 136).replace(/\0.*$/, '').trim(), 8);
  const body = Buffer.alloc(size);
  readSync(tarFd, body, 0, size, 512);
  closeSync(tarFd);
  return new Uint8Array(body);
})();

Module.tileFetch = (url) => {
  if (!url.startsWith(TILE_URL_PREFIX)) return { httpCode: 404, body: null };
  const relative = url.slice(TILE_URL_PREFIX.length);
  tileRequests.push(relative);
  if (relative === 'index.bin') return { httpCode: 200, body: indexBin };
  try {
    return { httpCode: 200, body: new Uint8Array(readFileSync(join(tileDir, relative))) };
  } catch {
    return { httpCode: 404, body: null };
  }
};

const perTileConfig = JSON.parse(config);
delete perTileConfig.mjolnir.tile_dir;
perTileConfig.mjolnir.tile_url = `${TILE_URL_PREFIX}{tilePath}`;

const perTileActor = new Module.Actor(JSON.stringify(perTileConfig));
const perTileRoute = JSON.parse(
  perTileActor.route(
    JSON.stringify({
      locations: [
        { lat: 47.141, lon: 9.521 },
        { lat: 47.165, lon: 9.51 },
      ],
      costing: 'auto',
    }),
  ),
);
assert.equal(tileRequests[0], 'index.bin', 'the reader did not look for an index.bin');
assert(tileRequests.length > 1, 'no per-tile requests were made');
assert(
  tileRequests.slice(1).every((r) => /^\d+(\/\d{3})+\.gph$/.test(r)),
  `per-tile requests are not tile paths: ${tileRequests.join(', ')}`,
);
assert.deepEqual(perTileRoute.trip.summary, summary, 'per-tile route differs from the NODEFS route');

// loki only builds a connectivity map, and so only reports a bbox, when it knows the tileset
const perTileStatus = JSON.parse(perTileActor.status('{"verbose":true}'));
assert(perTileStatus.bbox, 'an indexed per-tile tileset must keep loki.use_connectivity on');

perTileActor.delete();
console.log(`per-tile route matches, ${tileRequests.length - 1} tile requests`);

console.log('OK');
