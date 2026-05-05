#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
REPO_URL="${KANTAN_MUSIC_REPO_URL:-https://github.com/U1Nagata/kantan-music}"
REF="${KANTAN_MUSIC_REF:-main}"
WORK_DIR="${KANTAN_MUSIC_WORK_DIR:-/private/tmp/kantan-music-wasm-source}"

if [[ -z "$WORK_DIR" || "$WORK_DIR" == "/" ]]; then
  echo "error: unsafe KANTAN_MUSIC_WORK_DIR" >&2
  exit 1
fi

rm -rf "$WORK_DIR"
git clone --depth 1 --no-checkout --branch "$REF" "$REPO_URL" "$WORK_DIR"
git -C "$WORK_DIR" -c filter.lfs.process= -c filter.lfs.smudge= -c filter.lfs.required=false \
  checkout HEAD -- core/include/KANTANMusic.h core/src/KANTANMusic.c

"$ROOT_DIR/tools/kantan-music-wasm/build.sh" "$WORK_DIR/core/src/KANTANMusic.c"
