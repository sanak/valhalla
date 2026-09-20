// Main-thread proxy. Every action crosses to the worker and comes back as a promise.
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
      const settle = this.#inflight;
      if (!settle || settle.id !== data.id) return;
      this.#inflight = null;
      // structured clone drops the prototype, so the error is rebuilt here
      data.ok ? settle.resolve(data.result) : settle.reject(new ValhallaError(data.error));
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
    for (const { reject } of doomed) {
      reject(new ValhallaError({ message }));
    }
  }

  #send(action, payload) {
    if (this.#dead) {
      return Promise.reject(new ValhallaError({ message: this.#dead }));
    }
    const id = this.#nextId++;
    return new Promise((resolve, reject) => {
      this.#queued.push({ id, action, payload, resolve, reject });
      this.#pump();
    });
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

  route(request) { return this.#send('route', request); }
  locate(request) { return this.#send('locate', request); }
  matrix(request) { return this.#send('matrix', request); }
  optimizedRoute(request) { return this.#send('optimizedRoute', request); }
  isochrone(request) { return this.#send('isochrone', request); }
  traceRoute(request) { return this.#send('traceRoute', request); }
  traceAttributes(request) { return this.#send('traceAttributes', request); }
  height(request) { return this.#send('height', request); }
  transitAvailable(request) { return this.#send('transitAvailable', request); }
  expansion(request) { return this.#send('expansion', request); }
  centroid(request) { return this.#send('centroid', request); }
  status(request) { return this.#send('status', request); }
}
