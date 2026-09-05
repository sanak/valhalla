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
  #pending = new Map();
  #nextId = 0;

  constructor(worker) {
    this.#worker = worker;
    worker.onmessage = ({ data }) => {
      const settle = this.#pending.get(data.id);
      if (!settle) return;
      this.#pending.delete(data.id);
      // structured clone drops the prototype, so the error is rebuilt here
      data.ok ? settle.resolve(data.result) : settle.reject(new ValhallaError(data.error));
    };
  }

  #send(action, payload) {
    const id = this.#nextId++;
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#worker.postMessage({ id, action, payload });
    });
  }

  static async create({ workerUrl, config, cacheDir = null }) {
    const worker = new Worker(workerUrl, { type: 'module' });
    const instance = new Valhalla(worker);
    await instance.#send('init', { config, cacheDir });
    return instance;
  }

  terminate() {
    this.#worker.terminate();
    for (const { reject } of this.#pending.values()) {
      reject(new ValhallaError({ message: 'worker terminated' }));
    }
    this.#pending.clear();
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
