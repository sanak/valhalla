#!/usr/bin/env bash
# Shared environment for the valhalla wasm build. Sourced, not executed; safe in bash and zsh.
# Override any of WASM_BUILD_ROOT / WASM_EMSDK / HOST_PROTOC / WASM_PYTHON / BOOST_INCLUDE by
# exporting it beforehand -- a `VAR=x source env.sh` prefix does not survive the call in bash.

_wasm_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export VALHALLA_SRC="${VALHALLA_SRC:-$(cd "${_wasm_dir}/../../.." && pwd)}"

set -a
. "${_wasm_dir}/versions.env"
set +a

export WASM_BUILD_ROOT="${WASM_BUILD_ROOT:-${VALHALLA_SRC}/build-wasm}"
export PREFIX="${WASM_BUILD_ROOT}/prefix"
# the odin locales step imports polib; keep it out of the system python
export WASM_PYTHON="${WASM_PYTHON:-${WASM_BUILD_ROOT}/venv-polib/bin/python3}"
BOOST_ROOT_INCLUDE="${BOOST_INCLUDE:-$(brew --prefix boost 2>/dev/null || echo /usr)/include}"
# every object in the link must agree on the exception scheme
export EH_FLAGS="-fwasm-exceptions"
# not EMSDK_*: `emsdk construct_env` unsets every EMSDK_ variable it does not own
export WASM_EMSDK="${WASM_EMSDK:-${WASM_BUILD_ROOT}/emsdk}"

mkdir -p "${WASM_BUILD_ROOT}" "${PREFIX}"

# CMake turns Boost_INCLUDE_DIR into an -isystem for every object, so it must name a directory
# holding nothing but boost/. On linux that is /usr/include, where emscripten's libc++ then finds
# glibc's stdint.h and the build dies on bits/libc-header-start.h.
if [ -d "${BOOST_ROOT_INCLUDE}/boost" ]; then
  mkdir -p "${WASM_BUILD_ROOT}/boost-include"
  ln -sfn "${BOOST_ROOT_INCLUDE}/boost" "${WASM_BUILD_ROOT}/boost-include/boost"
  export BOOST_INCLUDE="${WASM_BUILD_ROOT}/boost-include"
else
  export BOOST_INCLUDE="${BOOST_ROOT_INCLUDE}"
fi

if [ ! -f "${WASM_EMSDK}/emsdk_env.sh" ]; then
  echo "### installing emsdk ${EMSDK_VERSION} into ${WASM_EMSDK}"
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "${WASM_EMSDK}"
  # emsdk prefers its own bundled python, which can be older than the 3.10 it requires
  EMSDK_PYTHON="$(command -v python3)" "${WASM_EMSDK}/emsdk" install "${EMSDK_VERSION}"
  EMSDK_PYTHON="$(command -v python3)" "${WASM_EMSDK}/emsdk" activate "${EMSDK_VERSION}"
fi

. "${WASM_EMSDK}/emsdk_env.sh" > /dev/null 2>&1 || :
if ! command -v emcc > /dev/null; then
  echo "env.sh: ${WASM_EMSDK}/emsdk_env.sh did not put emcc on PATH; run './emsdk activate'" >&2
  return 1 2>/dev/null || exit 1
fi

# emsdk construct_env unsets every EMSDK_ variable it does not own, EMSDK_VERSION included
set -a
. "${_wasm_dir}/versions.env"
set +a

_have="$(tr -d '"' < "${WASM_EMSDK}/upstream/emscripten/emscripten-version.txt" 2>/dev/null || true)"
if [ "${_have}" != "${EMSDK_VERSION}" ]; then
  echo "env.sh: warning: emscripten ${_have} active, versions.env pins ${EMSDK_VERSION}" >&2
fi
unset _have _wasm_dir

# our cross-built deps come first, the emscripten sysroot second
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="${PREFIX}:${CMAKE_PREFIX_PATH:-}"
