export declare class ValhallaError extends Error {
  name: 'ValhallaError';
  code?: number;
  httpCode?: number;
}

export interface ValhallaOptions {
  /** URL of worker.js, resolved by the page. */
  workerUrl: string | URL;
  /** Valhalla config. Set mjolnir.tile_url to a remote tar to range-fetch tiles. */
  config: Record<string, unknown>;
  /** Mount point for an IDBFS tile cache. Omit to keep tiles in memory only. */
  cacheDir?: string | null;
}

export declare class Valhalla {
  static create(options: ValhallaOptions): Promise<Valhalla>;
  route(request: string): Promise<string>;
  locate(request: string): Promise<string>;
  matrix(request: string): Promise<string>;
  optimizedRoute(request: string): Promise<string>;
  isochrone(request: string): Promise<string>;
  traceRoute(request: string): Promise<string>;
  traceAttributes(request: string): Promise<string>;
  height(request: string): Promise<string>;
  transitAvailable(request: string): Promise<string>;
  expansion(request: string): Promise<string>;
  centroid(request: string): Promise<string>;
  status(request: string): Promise<string>;
  terminate(): void;
}
