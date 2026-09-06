export declare class ValhallaError extends Error {
  name: 'ValhallaError';
  code?: number;
  httpCode?: number;
}

export interface RequestOptions {
  /** Abort the request. Rejects with a DOMException whose name is 'AbortError'. */
  signal?: AbortSignal;
}

export interface ValhallaOptions {
  /** URL of worker.js, resolved by the page. */
  workerUrl: string | URL;
  /** Valhalla config. Set mjolnir.tile_url to a remote tar to range-fetch tiles. */
  config: Record<string, unknown>;
  /** Mount point for an IDBFS tile cache. Omit to keep tiles in memory only. */
  cacheDir?: string | null;
  /**
   * How long to wait for the worker to acknowledge an abort before terminating and
   * restarting it. Defaults to 3000. Only reached when a synchronous tile fetch outlasts
   * the interrupt checks; without SharedArrayBuffer every abort restarts the worker.
   */
  abortTimeoutMs?: number;
}

export declare class Valhalla {
  static create(options: ValhallaOptions): Promise<Valhalla>;
  route(request: string, options?: RequestOptions): Promise<string>;
  locate(request: string, options?: RequestOptions): Promise<string>;
  matrix(request: string, options?: RequestOptions): Promise<string>;
  optimizedRoute(request: string, options?: RequestOptions): Promise<string>;
  isochrone(request: string, options?: RequestOptions): Promise<string>;
  traceRoute(request: string, options?: RequestOptions): Promise<string>;
  traceAttributes(request: string, options?: RequestOptions): Promise<string>;
  height(request: string, options?: RequestOptions): Promise<string>;
  transitAvailable(request: string, options?: RequestOptions): Promise<string>;
  expansion(request: string, options?: RequestOptions): Promise<string>;
  centroid(request: string, options?: RequestOptions): Promise<string>;
  status(request: string, options?: RequestOptions): Promise<string>;
  terminate(): void;
}
