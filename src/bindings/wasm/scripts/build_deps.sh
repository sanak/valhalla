#!/usr/bin/env bash
set -o errexit -o pipefail -o nounset
# Downloads protoc and cross-builds zlib + protobuf-lite (with fetched abseil) for
# wasm32-emscripten. Idempotent: each stage no-ops once its output exists.
source "$(dirname "$0")/env.sh"
cd "${WASM_BUILD_ROOT}"

echo "### emcc: $(emcc --version | sed -n 1p)"

# ---- protoc --------------------------------------------------------------
# Downloaded rather than taken from the system, so it cannot drift from the wasm runtime:
# protoc stamps `#if PROTOBUF_VERSION != <n>` into every generated .pb.h, and a mismatch
# surfaces ~10 minutes into build.sh as a dozen "incompatible version" errors.
PROTOC_DIR="${WASM_BUILD_ROOT}/protoc"
if [ -z "${HOST_PROTOC:-}" ]; then
  if [ ! -x "${PROTOC_DIR}/bin/protoc" ]; then
    case "$(uname -s)-$(uname -m)" in
      Darwin-arm64)   ZIP="protoc-${PROTOBUF_VERSION}-osx-aarch_64.zip" ;;
      Darwin-x86_64)  ZIP="protoc-${PROTOBUF_VERSION}-osx-x86_64.zip" ;;
      Linux-x86_64)   ZIP="protoc-${PROTOBUF_VERSION}-linux-x86_64.zip" ;;
      Linux-aarch64)  ZIP="protoc-${PROTOBUF_VERSION}-linux-aarch_64.zip" ;;
      *) echo "build_deps.sh: no protoc release for $(uname -s)-$(uname -m); set HOST_PROTOC" >&2
         exit 1 ;;
    esac
    echo "### downloading ${ZIP}"
    mkdir -p "${PROTOC_DIR}"
    curl -fsSL -o "${PROTOC_DIR}/${ZIP}" \
      "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION}/${ZIP}"
    unzip -oq "${PROTOC_DIR}/${ZIP}" -d "${PROTOC_DIR}"
    chmod +x "${PROTOC_DIR}/bin/protoc"
  fi
  HOST_PROTOC="${PROTOC_DIR}/bin/protoc"
fi
export HOST_PROTOC

HAVE="$("${HOST_PROTOC}" --version | awk '{print $2}')"
if [ "${HAVE}" != "${PROTOBUF_VERSION}" ]; then
  echo "build_deps.sh: HOST_PROTOC is ${HAVE}, versions.env pins ${PROTOBUF_VERSION}" >&2
  echo "  unset HOST_PROTOC to use the downloaded one, or bump versions.env" >&2
  exit 1
fi
echo "### protoc: ${HOST_PROTOC} (${HAVE})"

# ---- sources -------------------------------------------------------------
if [ ! -d "${WASM_BUILD_ROOT}/zlib/.git" ]; then
  git clone --depth 1 --branch "v${ZLIB_VERSION}" https://github.com/madler/zlib.git \
    "${WASM_BUILD_ROOT}/zlib"
fi
if [ ! -d "${WASM_BUILD_ROOT}/protobuf/.git" ]; then
  git clone --depth 1 --branch "v${PROTOBUF_VERSION}" --recurse-submodules --shallow-submodules \
    https://github.com/protocolbuffers/protobuf.git "${WASM_BUILD_ROOT}/protobuf"
fi

# ---- zlib ----------------------------------------------------------------
if [ ! -f "${PREFIX}/lib/libz.a" ]; then
  echo "### building zlib"
  emcmake cmake -S zlib -B build-zlib -G Ninja \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DZLIB_BUILD_EXAMPLES=OFF
  cmake --build build-zlib --target install
  # emscripten cannot link the fake .so; force -lz onto the static archive
  rm -f "${PREFIX}"/lib/libz.so*
else
  echo "### zlib already installed"
fi

# ---- protobuf (+ abseil via FetchContent) --------------------------------
# FORCE_FETCH_DEPENDENCIES is essential: without it protobuf finds a host abseil and drags
# native objects into the wasm link.
if [ ! -f "${PREFIX}/lib/libprotobuf-lite.a" ]; then
  echo "### building protobuf-lite"
  emcmake cmake -S protobuf -B build-protobuf -G Ninja \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_CXX_FLAGS="${EH_FLAGS}" \
    -DBUILD_SHARED_LIBS=OFF \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_CONFORMANCE=OFF \
    -Dprotobuf_BUILD_EXAMPLES=OFF \
    -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
    -Dprotobuf_BUILD_LIBUPB=OFF \
    -Dprotobuf_WITH_ZLIB=OFF \
    -Dprotobuf_FORCE_FETCH_DEPENDENCIES=ON \
    -Dprotobuf_INSTALL=ON
  cmake --build build-protobuf --target install
else
  echo "### protobuf already installed"
fi

echo "### deps done"
ls "${PREFIX}/lib"
