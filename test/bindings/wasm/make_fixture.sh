#!/usr/bin/env bash
# Builds the remote-tar fixture the wasm tests range-fetch from.
#   ./make_fixture.sh <tile_dir> <out.tar> [extra valhalla_build_extract args, e.g. --gzip]
set -o errexit -o pipefail -o nounset

TILE_DIR="${1:?usage: make_fixture.sh <tile_dir> <out.tar>}"
OUT_TAR="${2:?usage: make_fixture.sh <tile_dir> <out.tar>}"
REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"

python3 "${REPO_ROOT}/scripts/valhalla_build_extract" \
  -i "{\"mjolnir\":{\"tile_dir\":\"${TILE_DIR}\",\"tile_extract\":\"${OUT_TAR}\"}}" -O "${@:3}"
