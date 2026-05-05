#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$ROOT_DIR/docs/ui/kantan-music"
INCLUDE_DIR="$ROOT_DIR/main/kantan-music/include"
WRAPPER="$ROOT_DIR/tools/kantan-music-wasm/kantan_music_wasm.c"

mkdir -p "$OUT_DIR"

if ! command -v emcc >/dev/null 2>&1; then
  echo "error: emcc not found. Install and activate Emscripten first." >&2
  exit 1
fi

inputs=("$WRAPPER")

if [[ -n "${KANTAN_MUSIC_WASM_LIB:-}" ]]; then
  inputs+=("$KANTAN_MUSIC_WASM_LIB")
elif [[ "$#" -gt 0 ]]; then
  inputs+=("$@")
else
  cat >&2 <<'MSG'
error: no KANTAN Music implementation was provided.

Provide either:
  KANTAN_MUSIC_WASM_LIB=/path/to/libkantan-music-wasm.a tools/kantan-music-wasm/build.sh

or pass source/object/library files explicitly:
  tools/kantan-music-wasm/build.sh /path/to/KANTANMusic.c

The existing native/ESP32 .a files cannot be linked into WebAssembly.
MSG
  exit 1
fi

emcc "${inputs[@]}" \
  -I "$INCLUDE_DIR" \
  -O3 \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=createKANTANMusicModule \
  -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_FUNCTIONS='["_kantan_music_get_midi_note_number"]' \
  -o "$OUT_DIR/kantan-music.js"

python3 "$ROOT_DIR/tools/kantan-music-wasm/patch_locate_file.py" "$OUT_DIR/kantan-music.js"
cp "$ROOT_DIR/main/kantan-music/LICENSE_KANTAN_MUSIC.md" "$OUT_DIR/LICENSE_KANTAN_MUSIC.md"

if [[ ! -s "$OUT_DIR/kantan-music.js" || ! -s "$OUT_DIR/kantan-music.wasm" ]]; then
  echo "error: Emscripten did not create kantan-music.js and kantan-music.wasm" >&2
  exit 1
fi

echo "wrote $OUT_DIR/kantan-music.js and $OUT_DIR/kantan-music.wasm"
