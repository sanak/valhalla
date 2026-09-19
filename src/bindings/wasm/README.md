# Valhalla WebAssembly bindings

The routing core (Loki, Thor, Odin, Tyr, Sif, Baldr, Midgard) compiled to
wasm32-emscripten with embind, usable from a browser or from Node. Tile building is out of
scope: Mjolnir needs LuaJIT, which has no wasm backend, so tiles are built natively and read
by the wasm module.

## Prerequisites

- CMake, Ninja, git, curl, unzip, Python 3
- Boost headers (`brew install boost` / `apt install libboost-dev`)
- Node — the Emscripten SDK ships one, exported as `$EMSDK_NODE` after sourcing `env.sh`

The Emscripten SDK, `protoc`, protobuf and zlib are installed by the scripts from the
versions pinned in `versions.env`. Nothing is taken from the system package manager.

`make_tiles.sh`'s native fallback additionally needs LuaJIT, GEOS, SQLite3, spatialite and
the spatialite command-line tools — only when `VALHALLA_BUILD_TILES` is not supplied.

## Build

    ./scripts/build_deps.sh    # protoc, zlib, protobuf-lite (~30 min cold, no-op warm)
    ./scripts/build.sh         # libvalhalla + valhalla.mjs / valhalla.wasm
    ./scripts/make_tiles.sh    # the Liechtenstein tileset the tests route over

Everything lands in `<repo>/build-wasm/`, which is gitignored and disposable — deleting it
costs a rebuild, not a recipe. Override the location with `WASM_BUILD_ROOT`.

Already have a native valhalla build? Skip the one `make_tiles.sh` would do:

    VALHALLA_BUILD_TILES=<repo>/build/valhalla_build_tiles ./scripts/make_tiles.sh

Already have an Emscripten SDK? Point at it instead of installing a second copy:

    export WASM_EMSDK=/path/to/emsdk

## Test

    cd ../../../test/bindings/wasm && npm install --no-package-lock && npx playwright install chromium
    cd -
    source scripts/env.sh
    VALHALLA_WASM=$WASM_BUILD_ROOT/build-valhalla/src/bindings/wasm/valhalla.mjs \
    VALHALLA_TILE_DIR=$WASM_BUILD_ROOT/tiles \
      "$EMSDK_NODE" ../../../test/bindings/wasm/tests.mjs

    VALHALLA_WASM_DIR=$WASM_BUILD_ROOT/build-valhalla/src/bindings/wasm \
    VALHALLA_TILE_DIR=$WASM_BUILD_ROOT/tiles \
      "$EMSDK_NODE" ../../../test/bindings/wasm/browser/test_browser.mjs

The node test routes Vaduz to Schaan over NODEFS-mounted tiles and again over a tar reached
through range requests. The browser test serves the module and a tar to headless chromium and
asserts that an IDBFS cache cuts the range requests on a second load.

## Cancellation

Every action takes a second, optional argument `{ signal }` where `signal` is a standard
`AbortSignal`. Aborting it rejects the call with a `DOMException` named `'AbortError'`. Calls
are serialised — one action in flight at a time, the rest queued on the main thread — so an
abort on a queued call never touches the worker: it is spliced out of the queue and rejected
directly.

Aborting the in-flight call is cooperative when possible. If a `SharedArrayBuffer` could be
allocated, the main thread flips a flag in shared memory that the wasm module polls from
inside valhalla's expansion loops, so the pathfinder unwinds and the worker survives with its
warm tile cache. Getting a `SharedArrayBuffer` in a browser requires the page to be served with
`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.

**`require-corp` has a consequence beyond this binding: every cross-origin subresource the
page loads must then be fetched in CORS mode or carry `Cross-Origin-Resource-Policy`.** This
binding's whole point is range-fetching tiles from a remote host, so turning on isolation to
get cooperative cancellation can silently break tile fetching — check the tile host's response
headers before enabling COOP/COEP.

Without a `SharedArrayBuffer`, or when the worker doesn't acknowledge the abort within
`abortTimeoutMs` (default 3000, only reached when a synchronous tile fetch outlasts the
interrupt checks), the worker is terminated and rebuilt: the in-memory tile cache is lost, but
an IDBFS cache under `cacheDir` survives since it lives in IndexedDB, not in the worker. Every
in-flight abort restarts the worker this way when `SharedArrayBuffer` is unavailable,
regardless of `abortTimeoutMs`.

## Pitfalls

Each of these cost a build cycle:

- **`protoc` and the wasm protobuf runtime must be the same release, patch number included.**
  protoc stamps `#if PROTOBUF_VERSION != <n>` into every generated `.pb.h`. `versions.env`
  fixes both, so leave `HOST_PROTOC` unset unless you know the versions agree.
- **Every object needs `-fwasm-exceptions`**, protobuf and abseil included, or the link fails.
- **`loki.use_connectivity` needs a tileset listing.** The connectivity map only needs the list of
  tiles, which `GetTileSet()` takes from `index.bin`: a remote tar carries one, and a `{tilePath}`
  URL gets one when `index.bin` is published next to the tiles. Without it the map comes out empty
  and loki rejects every route with a 170, so the `Actor` constructor in `src/valhalla_wasm.cc`
  forces it off when `GetTileSet()` is empty; `valhalla_build_config` always emits it as `true`.
  Serve that `index.bin` uncompressed — the reader parses its bytes directly and never runs them
  through the `tile_url_gz` path.
- **Throw `valhalla_exception_t` from a tile getter, never a bare `std::exception`.** Loki
  turns the latter into a 171 and the real cause is lost. The catch sites in `src/loki/*_action.cc`
  rethrow `valhalla_exception_t` unchanged so this holds for every action, not just `/route`.
- **Never hand-write a valhalla config.** `worker_t` reads keys with `ptree.get<T>()` and no
  defaults, so a missing one throws `ptree_bad_path`. Generate it with
  `<repo>/scripts/valhalla_build_config` and override only what you need — that is what
  `test/bindings/wasm/valhalla.json` is.
- **Formatting needs clang-format-11 exactly.** Newer versions produce different output.
- **Do not name a variable `EMSDK_*`.** `emsdk construct_env` emits `unset` for every `EMSDK_`
  variable it does not own, so sourcing `emsdk_env.sh` silently blanks it. Hence `WASM_EMSDK`.
