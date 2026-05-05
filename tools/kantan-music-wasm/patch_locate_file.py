#!/usr/bin/env python3
"""Patch the Emscripten factory to resolve .wasm next to kantan-music.js."""

from __future__ import annotations

import pathlib
import sys


PATCH = """\
// KANTAN Play loader patch: resolve kantan-music.wasm relative to this script.
var __kantanMusicScriptUrl = (typeof document !== 'undefined' && document.currentScript && document.currentScript.src) ? document.currentScript.src : '';
var __kantanMusicBaseUrl = __kantanMusicScriptUrl ? new URL('.', __kantanMusicScriptUrl).href : '';
var __kantanMusicFactory = createKANTANMusicModule;
createKANTANMusicModule = function(Module) {
  Module = Module || {};
  var userLocateFile = Module.locateFile;
  Module.locateFile = function(path, prefix) {
    if (path.endsWith('.wasm') && __kantanMusicBaseUrl) return __kantanMusicBaseUrl + path;
    return userLocateFile ? userLocateFile(path, prefix) : prefix + path;
  };
  return __kantanMusicFactory(Module);
};
"""


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_locate_file.py path/to/kantan-music.js", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")
    marker = "KANTAN Play loader patch"
    if marker in text:
        return 0
    path.write_text(text + "\n" + PATCH, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
