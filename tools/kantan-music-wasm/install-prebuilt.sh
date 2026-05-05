#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$ROOT_DIR/docs/ui/kantan-music"

if [[ "$#" -ne 1 ]]; then
  cat >&2 <<'MSG'
usage: tools/kantan-music-wasm/install-prebuilt.sh /path/to/prebuilt-dir

The prebuilt directory must contain:
  kantan-music.js
  kantan-music.wasm
MSG
  exit 2
fi

SRC_DIR="$1"
JS="$SRC_DIR/kantan-music.js"
WASM="$SRC_DIR/kantan-music.wasm"

if [[ ! -s "$JS" || ! -s "$WASM" ]]; then
  echo "error: prebuilt directory must contain non-empty kantan-music.js and kantan-music.wasm" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
cp "$JS" "$OUT_DIR/kantan-music.js"
cp "$WASM" "$OUT_DIR/kantan-music.wasm"
python3 "$ROOT_DIR/tools/kantan-music-wasm/patch_locate_file.py" "$OUT_DIR/kantan-music.js"
cp "$ROOT_DIR/main/kantan-music/LICENSE_KANTAN_MUSIC.md" "$OUT_DIR/LICENSE_KANTAN_MUSIC.md"
perl -0pi -e 's/\r\n/\n/g; s/\n+\z/\n/' "$OUT_DIR/LICENSE_KANTAN_MUSIC.md"

echo "installed $OUT_DIR/kantan-music.js and $OUT_DIR/kantan-music.wasm"
