// Main-thread proxy. Every action crosses to the worker and comes back as a promise.
// DOMException is the shape fetch() uses, so callers branch on err.name like they already do
function abortError() {
  return new DOMException('request cancelled', 'AbortError');
}

// Feature-detect rather than reading crossOriginIsolated: node has no such global but always
// has SharedArrayBuffer, and a browser without cross-origin isolation has neither.
function makeCancelFlag() {
  try {
    return new Int32Array(new SharedArrayBuffer(4));
  } catch {
    return null;
  }
}

export class ValhallaError extends Error {
  constructor({ message, code, httpCode }) {
    super(message);
    this.name = 'ValhallaError';
    this.code = code;
    this.httpCode = httpCode;
  }
}

export class Valhalla {
  #worker;
  #workerUrl;
  #initPayload = null;
  #inflight = null;
  #pendingInit = null;
  #queued = [];
  #nextId = 1;
  #dead = null;
  #restarting = false;
  #abortTimeoutMs = 3000;
  #flag = makeCancelFlag();

  constructor(workerUrl) {
    this.#workerUrl = workerUrl;
    this.#spawn();
  }

  #spawn() {
    this.#worker = new Worker(this.#workerUrl, { type: 'module' });
    this.#worker.onmessage = ({ data }) => {
      // init bypasses the queue so a respawn can unblock it, so it settles from its own slot
      const boot = this.#pendingInit;
      if (boot && boot.id === data.id) {
        this.#pendingInit = null;
        data.ok ? boot.resolve() : boot.reject(new ValhallaError(data.error));
        return;
      }
      const entry = this.#inflight;
      if (!entry || entry.id !== data.id) return;
      this.#inflight = null;
      clearTimeout(entry.timer);
      if ((entry.aborting || data.aborted) && this.#flag) Atomics.store(this.#flag, 0, 0);
      this.#settle(entry, () => {
        // a result that beat the abort still rejects, so a cancelled call has one outcome
        if (data.aborted || entry.aborting) {
          entry.reject(abortError());
        } else if (data.ok) {
          entry.resolve(data.result);
        } else {
          // structured clone drops the prototype, so the error is rebuilt here
          entry.reject(new ValhallaError(data.error));
        }
      });
      this.#pump();
    };
    // a worker that fails to load or throws at top level never posts back, so without this
    // every pending call - create() included - would hang instead of reporting the failure
    this.#worker.onerror = (e) => this.#kill(e.message ?? 'worker failed to start');
    this.#worker.onmessageerror = () => this.#kill('worker sent an uncloneable message');
  }

  #init() {
    return new Promise((resolve, reject) => {
      const id = this.#nextId++;
      this.#pendingInit = { id, resolve, reject };
      this.#worker.postMessage({ id, action: 'init', payload: this.#initPayload });
    });
  }

  #kill(message) {
    this.#dead ??= message;
    this.#restarting = false;
    const doomed = [this.#pendingInit, this.#inflight, ...this.#queued].filter(Boolean);
    this.#pendingInit = null;
    this.#inflight = null;
    this.#queued = [];
    for (const entry of doomed) {
      clearTimeout(entry.timer);
      this.#settle(entry, () => entry.reject(new ValhallaError({ message })));
    }
  }

  #settle(entry, fn) {
    if (entry.onAbort) entry.signal?.removeEventListener('abort', entry.onAbort);
    fn();
  }

  #send(action, payload, options) {
    const signal = options?.signal;
    if (this.#dead) {
      return Promise.reject(new ValhallaError({ message: this.#dead }));
    }
    if (signal?.aborted) {
      return Promise.reject(abortError());
    }
    const id = this.#nextId++;
    return new Promise((resolve, reject) => {
      const entry = { id, action, payload, resolve, reject, signal };
      if (signal) {
        entry.onAbort = () => this.#abort(id);
        signal.addEventListener('abort', entry.onAbort, { once: true });
      }
      this.#queued.push(entry);
      this.#pump();
    });
  }

  #abort(id) {
    const i = this.#queued.findIndex((e) => e.id === id);
    if (i !== -1) {
      const [entry] = this.#queued.splice(i, 1);
      entry.reject(abortError());
      return;
    }
    const entry = this.#inflight;
    if (entry?.id !== id) return;
    entry.aborting = true;
    if (this.#flag) {
      Atomics.store(this.#flag, 0, id);
    }
    // a synchronous XHR can outlast every interrupt check, so the flag gets a deadline
    entry.timer = setTimeout(() => this.#respawn(), this.#flag ? this.#abortTimeoutMs : 0);
  }

  // the worker cannot be interrupted out of a synchronous fetch, so it is replaced wholesale
  async #respawn() {
    this.#restarting = true;
    const aborted = this.#inflight;
    this.#inflight = null;
    this.#worker.terminate();
    if (aborted) {
      clearTimeout(aborted.timer);
      this.#settle(aborted, () => aborted.reject(abortError()));
    }
    if (this.#flag) {
      Atomics.store(this.#flag, 0, 0);
    }
    try {
      this.#spawn();
      await this.#init();
    } catch (e) {
      this.#restarting = false;
      this.#kill(e.message ?? 'worker failed to restart');
      return;
    }
    this.#restarting = false;
    this.#pump();
  }

  // one action at a time: the worker owns a single actor, and a queue the main thread holds
  // is a queue it can still edit after the worker has stopped answering
  #pump() {
    if (this.#restarting || this.#inflight || this.#queued.length === 0) return;
    this.#inflight = this.#queued.shift();
    const { id, action, payload } = this.#inflight;
    this.#worker.postMessage({ id, action, payload });
  }

  static async create({ workerUrl, config, cacheDir = null, abortTimeoutMs = 3000 }) {
    const instance = new Valhalla(workerUrl);
    instance.#abortTimeoutMs = abortTimeoutMs;
    instance.#initPayload = { config, cacheDir, cancelFlag: instance.#flag };
    await instance.#init();
    return instance;
  }

  terminate() {
    this.#worker.terminate();
    this.#kill('worker terminated');
  }

  route(request, options) { return this.#send('route', request, options); }
  locate(request, options) { return this.#send('locate', request, options); }
  matrix(request, options) { return this.#send('matrix', request, options); }
  optimizedRoute(request, options) { return this.#send('optimizedRoute', request, options); }
  isochrone(request, options) { return this.#send('isochrone', request, options); }
  traceRoute(request, options) { return this.#send('traceRoute', request, options); }
  traceAttributes(request, options) { return this.#send('traceAttributes', request, options); }
  height(request, options) { return this.#send('height', request, options); }
  transitAvailable(request, options) { return this.#send('transitAvailable', request, options); }
  expansion(request, options) { return this.#send('expansion', request, options); }
  centroid(request, options) { return this.#send('centroid', request, options); }
  status(request, options) { return this.#send('status', request, options); }
}
