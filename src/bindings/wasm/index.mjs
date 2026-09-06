// Main-thread proxy. Every action crosses to the worker and comes back as a promise.
// DOMException is the shape fetch() uses, so callers branch on err.name like they already do
function abortError() {
  return new DOMException('request cancelled', 'AbortError');
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
  #inflight = null;
  #queued = [];
  #nextId = 1;
  #dead = null;

  constructor(worker) {
    this.#worker = worker;
    worker.onmessage = ({ data }) => {
      const entry = this.#inflight;
      if (!entry || entry.id !== data.id) return;
      this.#inflight = null;
      this.#settle(entry, () => {
        // structured clone drops the prototype, so the error is rebuilt here
        data.ok ? entry.resolve(data.result) : entry.reject(new ValhallaError(data.error));
      });
      this.#pump();
    };
    // a worker that fails to load or throws at top level never posts back, so without this
    // every pending call - create() included - would hang instead of reporting the failure
    worker.onerror = (e) => this.#kill(e.message ?? 'worker failed to start');
    worker.onmessageerror = () => this.#kill('worker sent an uncloneable message');
  }

  #kill(message) {
    this.#dead ??= message;
    const doomed = this.#inflight ? [this.#inflight, ...this.#queued] : this.#queued;
    this.#inflight = null;
    this.#queued = [];
    for (const entry of doomed) {
      this.#settle(entry, () => entry.reject(new ValhallaError({ message })));
    }
  }

  #settle(entry, fn) {
    if (entry.onAbort) entry.signal?.removeEventListener('abort', entry.onAbort);
    fn();
  }

  #send(action, payload, { signal } = {}) {
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
    if (this.#inflight?.id !== id) return;
    // Task 7 fills this in; until then an in-flight abort is a no-op
  }

  // one action at a time: the worker owns a single actor, and a queue the main thread holds
  // is a queue it can still edit after the worker has stopped answering
  #pump() {
    if (this.#inflight || this.#queued.length === 0) return;
    this.#inflight = this.#queued.shift();
    const { id, action, payload } = this.#inflight;
    this.#worker.postMessage({ id, action, payload });
  }

  static async create({ workerUrl, config, cacheDir = null }) {
    const worker = new Worker(workerUrl, { type: 'module' });
    const instance = new Valhalla(worker);
    await instance.#send('init', { config, cacheDir });
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
