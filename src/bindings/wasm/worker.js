// Runs the wasm module off the main thread. Synchronous XHR is only legal here, and the whole
// pathfinder blocks on each tile fetch, so nothing about this belongs on the main thread.
import createValhalla from './valhalla.mjs';

const ACTIONS = [
  'route', 'locate', 'matrix', 'optimizedRoute', 'isochrone', 'traceRoute',
  'traceAttributes', 'height', 'transitAvailable', 'expansion', 'centroid', 'status',
];

let Module = null;
let actor = null;
let cacheDir = null;
let bootConfig = null;
let cancelFlag = null;

function syncFetch(url, offset, size) {
  const xhr = new XMLHttpRequest();
  xhr.open('GET', url, false);
  xhr.responseType = 'arraybuffer';
  if (size > 0) {
    xhr.setRequestHeader('Range', `bytes=${offset}-${offset + size - 1}`);
  }
  xhr.send(null);
  return {
    httpCode: xhr.status,
    body: xhr.response ? new Uint8Array(xhr.response) : null,
  };
}

async function init(payload) {
  bootConfig = payload.config;
  cacheDir = payload.cacheDir ?? null;
  cancelFlag = payload.cancelFlag ?? null;
  Module = await createValhalla();
  Module.tileFetch = syncFetch;
  Module.cancelFlag = payload.cancelFlag ?? undefined;

  const config = structuredClone(bootConfig);
  if (cacheDir) {
    config.mjolnir.tile_dir = cacheDir;
    Module.FS.mkdirTree(cacheDir);
    Module.FS.mount(Module.IDBFS, {}, cacheDir);
    await new Promise((resolve, reject) => {
      // true = populate the wasm FS from IndexedDB
      Module.FS.syncfs(true, (err) => (err ? reject(err) : resolve()));
    });
  } else {
    delete config.mjolnir.tile_dir;
  }
  actor = new Module.Actor(JSON.stringify(config));
}

function serializeError(e) {
  return { message: e.message ?? String(e), code: e.code, httpCode: e.httpCode };
}

function flush() {
  if (!cacheDir) return Promise.resolve();
  // a syncfs with nothing dirty is cheap, so this runs unconditionally rather than tracking
  // whether an action actually fetched anything
  return new Promise((resolve, reject) => {
    Module.FS.syncfs(false, (err) => (err ? reject(err) : resolve()));
  });
}

function clearCache(dir) {
  for (const entry of Module.FS.readdir(dir)) {
    if (entry === '.' || entry === '..') continue;
    const path = `${dir}/${entry}`;
    if (Module.FS.isDir(Module.FS.stat(path).mode)) {
      clearCache(path);
      Module.FS.rmdir(path);
    } else {
      Module.FS.unlink(path);
    }
  }
}

async function handle({ id, action, payload }) {
  try {
    if (action === 'init') {
      await init(payload);
      self.postMessage({ id, ok: true, result: null });
      return;
    }
    if (!ACTIONS.includes(action)) {
      throw new Error(`unknown action: ${action}`);
    }
    let result;
    Module.currentRequestId = id;
    try {
      result = actor[action](payload);
    } catch (e) {
      // 446: the remote tar was rebuilt, so everything cached under cacheDir is stale
      if (e.code !== 446 || !cacheDir) throw e;
      actor.delete();
      clearCache(cacheDir);
      await flush();
      await init({ config: bootConfig, cacheDir, cancelFlag });
      Module.currentRequestId = id;
      result = actor[action](payload);
    } finally {
      Module.currentRequestId = undefined;
    }
    await flush();
    self.postMessage({ id, ok: true, result });
  } catch (e) {
    if (e.name === 'AbortError') {
      self.postMessage({ id, ok: false, aborted: true });
      return;
    }
    self.postMessage({ id, ok: false, error: serializeError(e) });
  }
}

// Module and actor are worker globals that init() and the 446 recovery both replace, so two
// actions overlapping across an await would see a deleted actor or a half-mounted FS
let queue = Promise.resolve();

self.onmessage = ({ data }) => {
  queue = queue.then(() => handle(data));
};
