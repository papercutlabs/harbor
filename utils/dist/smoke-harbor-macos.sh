#!/usr/bin/env bash
# Run on the native producer and again against the extracted archive, without opening a profile.
set -euo pipefail
[[ $# == 1 ]] || { echo 'Expected one exact Harbor archive.' >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
# The package script owns archive creation; Office independently validates paths before extraction.
tar -xzf "$1" -C "${WORK}"
BIN="${WORK}/harbor-runtime/bin/harbor"
[[ "$(lipo -archs "${BIN}")" == arm64 ]] || { echo 'Harbor has the wrong CPU architecture.' >&2; exit 1; }
codesign --verify --strict "${BIN}"
MEDIA="${WORK}/harbor-runtime/bin/ffmpeg"
[[ "$(lipo -archs "${MEDIA}")" == arm64 ]]
codesign --verify --strict "${MEDIA}"
MEDIA_UNEXPECTED="$(otool -L "${MEDIA}" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/Frameworks/)' || true)"
[[ -z "${MEDIA_UNEXPECTED}" ]] || { echo "FFmpeg has unbundled dependencies: ${MEDIA_UNEXPECTED}" >&2; exit 1; }
PATH=/usr/bin:/bin "${MEDIA}" -version
DEMO="${WORK}/harbor-runtime/share/harbor/demo"
PATH=/usr/bin:/bin "${MEDIA}" -v error -nostdin -i "${DEMO}/harbor.png" -vf scale=4:4 -frames:v 1 -f rawvideo -pix_fmt rgb24 "${WORK}/image.rgb"
[[ "$(wc -c < "${WORK}/image.rgb" | tr -d ' ')" == 48 ]]
PATH=/usr/bin:/bin "${MEDIA}" -v error -nostdin -i "${DEMO}/morning.mp4" -frames:v 1 -f null -
PATH=/usr/bin:/bin "${MEDIA}" -v error -nostdin -i "${DEMO}/voice.wav" -vn -ar 44100 -ac 1 "${WORK}/audio.wav"
[[ -s "${WORK}/audio.wav" ]]
UNEXPECTED="$(otool -L "${BIN}" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/Frameworks/)' || true)"
[[ -z "${UNEXPECTED}" ]] || { echo "Harbor has unbundled dynamic dependencies: ${UNEXPECTED}" >&2; exit 1; }
# An isolated HOME makes an accidental profile open fail the bundle's clean-profile check.
mkdir "${WORK}/home"
HOME="${WORK}/home" XDG_CONFIG_HOME="${WORK}/home/config" "${BIN}" --version > "${WORK}/version"
python3 - "${WORK}" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
manifest = json.loads((root / 'harbor-runtime/manifest.json').read_text())
assert (root / 'version').read_text().startswith('Harbor ' + manifest['version'] + ' ')
assert not list((root / 'home').iterdir()), 'version check opened a profile'
print((root / 'version').read_text().strip())
PY
