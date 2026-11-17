#!/usr/bin/env bash
# Hosted/developer producer only. Office consumes the resulting bundle and never invokes this script.
set -euo pipefail
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || { echo 'Harbor packages require a macOS arm64 producer.' >&2; exit 1; }
REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
export MACOSX_DEPLOYMENT_TARGET=12.0
export JOBS="${JOBS:-2}"
DEPS="${NCHAT_DIST_CACHE:-${HOME}/.cache/nchat-dist}/deps-macos-arm64"
BUILD="${REPO_DIR}/build-dist/harbor-macos-arm64"
STAGE="${REPO_DIR}/dist/harbor-runtime"
"${REPO_DIR}/utils/dist/build-deps-macos.sh"
cmake -S "${REPO_DIR}" -B "${BUILD}" \
  -DCMAKE_BUILD_TYPE=Release -DHAS_STATIC_EXTLIBS=ON \
  -DHAS_HARBOR=ON -DHAS_TELEGRAM=ON -DHAS_WHATSAPP=OFF -DHAS_SIGNAL=OFF \
  -DHAS_DUMMY=OFF -DHAS_COREDUMP=OFF -DHAS_TESTS=OFF \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
  -DCMAKE_PREFIX_PATH="${DEPS}" -DNCURSES_ROOT_DIR="${DEPS}" \
  -DOPENSSL_ROOT_DIR="${DEPS}" -DSQLITE_ROOT_DIR="${DEPS}" \
  -DCMAKE_INSTALL_PREFIX=/ -DCMAKE_INSTALL_RPATH='@executable_path/../lib'
cmake --build "${BUILD}" --target harbor -j "${JOBS}"
rm -rf "${STAGE}"
DESTDIR="${STAGE}" cmake --install "${BUILD}" --component HarborRuntime
mkdir -p "${STAGE}/share/harbor"
cp "${DEPS}/share/misc/magic.mgc" "${STAGE}/share/harbor/magic.mgc"
"${REPO_DIR}/utils/sign" "${REPO_DIR}/src/nchat.entitlements" "${STAGE}/bin/harbor"
"${REPO_DIR}/utils/dist/build-harbor-media-macos.sh"
python3 "${REPO_DIR}/utils/dist/package-harbor.py" "${STAGE}" "${REPO_DIR}/dist"
"${REPO_DIR}/utils/dist/smoke-harbor-macos.sh" "${REPO_DIR}"/dist/harbor-*-macos-arm64.tar.gz
