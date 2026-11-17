#!/usr/bin/env bash
# A closed FFmpeg tool for Harbor's image/video decoding and voice conversion.
# dav1d is statically included; no GPL/nonfree components or Homebrew runtime libraries.
set -euo pipefail
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || { echo 'Harbor media packages require macOS arm64.' >&2; exit 1; }
REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE="${REPO_DIR}/dist/harbor-runtime"
DEPS="${NCHAT_DIST_CACHE:-${HOME}/.cache/nchat-dist}/deps-macos-arm64"
BUILD="${REPO_DIR}/build-dist/harbor-ffmpeg"
DAV1D_BUILD="${REPO_DIR}/build-dist/harbor-dav1d"
MEDIA_DOC="${STAGE}/share/doc/harbor/ffmpeg"
# Homebrew verifies the source archive against its formula checksum. This is a
# hosted producer input only; the exact archive and hash travel with the runtime.
# AV1/AVIF need dav1d's software decoder on Macs without AV1 hardware.
HOMEBREW_NO_AUTO_UPDATE=1 brew fetch --build-from-source dav1d
DAV1D_SOURCE="$(HOMEBREW_NO_AUTO_UPDATE=1 brew --cache --build-from-source dav1d)"
rm -rf "${DAV1D_BUILD}"
mkdir -p "${DAV1D_BUILD}" "${MEDIA_DOC}/dav1d"
tar -xf "${DAV1D_SOURCE}" -C "${DAV1D_BUILD}" --strip-components=1
export MACOSX_DEPLOYMENT_TARGET=12.0
meson setup "${DAV1D_BUILD}/build" "${DAV1D_BUILD}" --prefix="${DEPS}" \
  --default-library=static --buildtype=release -Denable_tools=false -Denable_tests=false
ninja -C "${DAV1D_BUILD}/build" install
cp "${DAV1D_SOURCE}" "${MEDIA_DOC}/dav1d/source.tar.xz"
cp "${DAV1D_BUILD}/COPYING" "${MEDIA_DOC}/dav1d/COPYING"
HOMEBREW_NO_AUTO_UPDATE=1 brew fetch --build-from-source ffmpeg
SOURCE="$(HOMEBREW_NO_AUTO_UPDATE=1 brew --cache --build-from-source ffmpeg)"
[[ -f "${SOURCE}" ]] || { echo 'Verified FFmpeg source cache is missing.' >&2; exit 1; }
rm -rf "${BUILD}"
mkdir -p "${BUILD}" "${STAGE}/bin" "${STAGE}/share/doc/harbor/ffmpeg"
tar -xf "${SOURCE}" -C "${BUILD}" --strip-components=1
cd "${BUILD}"
export PKG_CONFIG_PATH="${DEPS}/lib/pkgconfig"
./configure --prefix="${BUILD}/install" --cc=clang \
  --disable-autodetect --disable-shared --enable-static \
  --disable-programs --enable-ffmpeg --disable-doc --disable-debug --disable-x86asm \
  --disable-gpl --disable-nonfree --enable-zlib --enable-videotoolbox \
  --enable-libdav1d --pkg-config-flags=--static \
  --extra-cflags="-I${DEPS}/include -mmacosx-version-min=12.0" \
  --extra-ldflags="-L${DEPS}/lib -mmacosx-version-min=12.0"
make -j "${JOBS:-2}" ffmpeg
cp ffmpeg "${STAGE}/bin/ffmpeg"
codesign --force --sign - --timestamp=none "${STAGE}/bin/ffmpeg"
# Accompany the LGPL executable with its exact corresponding source and build
# configuration, rather than a new written source-offer commitment.
cp "${SOURCE}" "${STAGE}/share/doc/harbor/ffmpeg/source.tar.xz"
cp COPYING.LGPLv2.1 LICENSE.md "${STAGE}/share/doc/harbor/ffmpeg/"
cp ffbuild/config.log "${STAGE}/share/doc/harbor/ffmpeg/config.log"
"${STAGE}/bin/ffmpeg" -version > "${STAGE}/share/doc/harbor/ffmpeg/version.txt"
( cd "${MEDIA_DOC}" && shasum -a 256 source.tar.xz dav1d/source.tar.xz > source.sha256 )
cp "${REPO_DIR}/utils/dist/build-harbor-media-macos.sh" "${STAGE}/share/doc/harbor/ffmpeg/build.sh"
