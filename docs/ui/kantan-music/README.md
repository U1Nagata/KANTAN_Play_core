# KANTAN Music WebAssembly module

Place the browser build of the KANTAN Music API in this directory:

- `kantan-music.js`
- `kantan-music.wasm`

The file manager loads `kantan-music.js` on demand when the user exports a
Song JSON with progression data as an SMF file.

## Build

Install and activate Emscripten, then run:

```sh
tools/kantan-music-wasm/build.sh /path/to/KANTANMusic.c
```

or, when you already have a WebAssembly-compatible static library:

```sh
KANTAN_MUSIC_WASM_LIB=/path/to/libkantan-music-wasm.a tools/kantan-music-wasm/build.sh
```

The native, macOS, Linux, and ESP32 libraries under `main/kantan-music/` cannot
be linked into WebAssembly. Build the KANTAN Music implementation for wasm32.

The build script writes `kantan-music.js` and `kantan-music.wasm` here.
It also patches `kantan-music.js` so the `.wasm` file is loaded from the same
GitHub Pages directory as the JavaScript file, even when the file manager page
itself is served by the device.

If the KANTAN Music implementation is built in a separate private repository or
machine, copy the prebuilt pair into this repository with:

```sh
tools/kantan-music-wasm/install-prebuilt.sh /path/to/prebuilt-dir
```

After building, deploy or commit both generated files together:

```text
docs/ui/kantan-music/kantan-music.js
docs/ui/kantan-music/kantan-music.wasm
```

Then run the browser file manager and export a Song JSON that has
`progression` data. Without these generated files, the UI reports:

```text
KANTAN Music WASM is not installed
```

## Adapter interface

The default build exposes an Emscripten factory named
`createKANTANMusicModule()` and the wrapper function:

```c
unsigned char kantan_music_get_midi_note_number(
  int pitch,
  int degree,
  int key,
  int voicing,
  int modifier,
  int semitone_shift,
  int bass_degree,
  int bass_semitone_shift,
  int position,
  int minor_swap);
```

Alternatively, a custom JavaScript adapter may expose:

```js
window.KANTANMusic = {
  getMidiNoteNumber(pitch, degree, key, options) {
    // return MIDI note number 0..127
  }
};
```

The KANTAN Music API is distributed under the KANTAN Music API License, not the
repository-wide MIT license. See `LICENSE_KANTAN_MUSIC.md`.
