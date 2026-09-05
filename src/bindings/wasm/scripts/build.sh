#!/usr/bin/env bash
set -o errexit -o pipefail -o nounset
# Configures and builds libvalhalla (routing core only) plus the wasm bindings.
# resolved before the cd below, which leaves $0's relative path meaningless
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/env.sh"
cd "${WASM_BUILD_ROOT}"

BUILD_DIR="${WASM_BUILD_ROOT}/build-valhalla"

# same fallback as build_deps.sh: an explicit HOST_PROTOC wins, otherwise the downloaded one
HOST_PROTOC="${HOST_PROTOC:-${WASM_BUILD_ROOT}/protoc/bin/protoc}"
export HOST_PROTOC
if [ ! -x "${HOST_PROTOC}" ]; then
  echo "build.sh: no protoc at ${HOST_PROTOC}; run scripts/build_deps.sh first" >&2
  exit 1
fi

# odin's locales step (src/odin/CMakeLists.txt) imports polib and runs even for a
# routing-only build. A Homebrew python refuses `pip install` under PEP 668, so use a venv.
if [ ! -x "${WASM_PYTHON}" ]; then
  echo "### creating polib venv at ${WASM_BUILD_ROOT}/venv-polib"
  python3 -m venv "${WASM_BUILD_ROOT}/venv-polib"
  "${WASM_BUILD_ROOT}/venv-polib/bin/pip" install --quiet polib
fi

emcmake cmake -S "${VALHALLA_SRC}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="${EH_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="-L${PREFIX}/lib" \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_DATA_TOOLS=OFF \
  -DENABLE_SERVICES=OFF \
  -DENABLE_TOOLS=OFF \
  -DENABLE_HTTP=OFF \
  -DENABLE_PYTHON_BINDINGS=OFF \
  -DENABLE_NODE_BINDINGS=OFF \
  -DENABLE_WASM_BINDINGS=ON \
  -DENABLE_TESTS=OFF \
  -DENABLE_CCACHE=OFF \
  -DENABLE_GEOTIFF=OFF \
  -DENABLE_LZ4=OFF \
  -DENABLE_SINGLE_FILES_WERROR=OFF \
  -DPREFER_EXTERNAL_DEPS=OFF \
  -DBoost_INCLUDE_DIR="${BOOST_INCLUDE}" \
  -DProtobuf_PROTOC_EXECUTABLE="${HOST_PROTOC}" \
  -DPYTHON_INTERPRETER="${WASM_PYTHON}" \
  -DCMAKE_PROJECT_INCLUDE="${SCRIPT_DIR}/host_protoc.cmake" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
  -DProtobuf_DIR="${PREFIX}/lib/cmake/protobuf" \
  -Dabsl_DIR="${PREFIX}/lib/cmake/absl" \
  -Dutf8_range_DIR="${PREFIX}/lib/cmake/utf8_range" \
  "$@"

cmake --build "${BUILD_DIR}" --target valhalla_wasm
ls -la "${BUILD_DIR}/src/bindings/wasm/valhalla.mjs" "${BUILD_DIR}/src/bindings/wasm/valhalla.wasm"
