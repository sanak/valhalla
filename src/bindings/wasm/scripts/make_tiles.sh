#!/usr/bin/env bash
set -o errexit -o pipefail -o nounset
# Builds the Liechtenstein tileset the wasm tests route over.
#
# Set VALHALLA_BUILD_TILES to an existing valhalla_build_tiles binary to skip the native
# build entirely, e.g. VALHALLA_BUILD_TILES=build/valhalla_build_tiles ./make_tiles.sh
source "$(dirname "$0")/env.sh"

TILE_DIR="${WASM_BUILD_ROOT}/tiles"
PBF="${VALHALLA_SRC}/test/data/liechtenstein-latest.osm.pbf"

if [ -n "${VALHALLA_BUILD_TILES:-}" ]; then
  BUILDER="${VALHALLA_BUILD_TILES}"
else
  # A minimal native build: data tools only, and only the one target we need. No admin or
  # timezone database is built -- the smoke route does not need them, and skipping them keeps
  # spatialite out of the dependency set.
  NATIVE_DIR="${WASM_BUILD_ROOT}/build-native"
  BUILDER="${NATIVE_DIR}/valhalla_build_tiles"
  if [ ! -x "${BUILDER}" ]; then
    echo "### building valhalla_build_tiles natively (set VALHALLA_BUILD_TILES to skip)"
    # odin's locales step imports polib and runs even for a data-tools-only build
    if ! "${WASM_PYTHON}" -c 'import polib' 2> /dev/null; then
      echo "### creating polib venv at ${WASM_BUILD_ROOT}/venv-polib"
      python3 -m venv "${WASM_BUILD_ROOT}/venv-polib"
      "${WASM_BUILD_ROOT}/venv-polib/bin/pip" install --quiet polib
    fi
    # a native build must not see the wasm prefix env.sh puts on these
    CMAKE_PREFIX_PATH= PKG_CONFIG_PATH= cmake -S "${VALHALLA_SRC}" -B "${NATIVE_DIR}" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_DATA_TOOLS=ON \
      -DENABLE_SERVICES=OFF \
      -DENABLE_HTTP=OFF \
      -DENABLE_TESTS=OFF \
      -DENABLE_PYTHON_BINDINGS=OFF \
      -DENABLE_NODE_BINDINGS=OFF \
      -DENABLE_SINGLE_FILES_WERROR=OFF \
      -DPYTHON_INTERPRETER="${WASM_PYTHON}"
    cmake --build "${NATIVE_DIR}" --target valhalla_build_tiles
  fi
fi

if [ ! -x "${BUILDER}" ]; then
  echo "make_tiles.sh: ${BUILDER} is not executable" >&2
  exit 1
fi

rm -rf "${TILE_DIR}"
mkdir -p "${TILE_DIR}"
"${BUILDER}" --inline-config \
  "{\"mjolnir\":{\"tile_dir\":\"${TILE_DIR}\",\"concurrency\":1,\"logging\":{\"type\":\"\"}}}" \
  "${PBF}"

echo "### tiles in ${TILE_DIR}"
find "${TILE_DIR}" -name '*.gph' | wc -l
