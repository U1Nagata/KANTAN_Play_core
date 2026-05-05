import fs from 'node:fs';
import vm from 'node:vm';

const appJs = fs.readFileSync('docs/ui/app.js', 'utf8');

const sandbox = {
  URL,
  Blob,
  console,
  location: { origin: 'http://localhost', href: 'http://localhost/ui/' },
  window: {},
  _elements: new Map(),
  document: {
    currentScript: { src: 'http://localhost/ui/app.js' },
    getElementById(id) {
      if (!sandbox._elements.has(id)) sandbox._elements.set(id, makeElement('div'));
      return sandbox._elements.get(id);
    },
    createElement(tag) {
      return makeElement(tag);
    },
    createTextNode(text) {
      return { text };
    },
    querySelectorAll() {
      return [];
    },
    head: { appendChild() {} },
    body: { appendChild() {} },
  },
  fetch: async () => ({ ok: true, headers: { get: () => 'application/json' }, json: async () => ({ files: [] }) }),
};
sandbox.window = sandbox;

function makeElement(tag) {
  return {
    tag,
    innerHTML: '',
    textContent: '',
    className: '',
    checked: false,
    indeterminate: false,
    disabled: false,
    dataset: {},
    style: {},
    setAttribute() {},
    appendChild() {},
    addEventListener() {},
  };
}

vm.runInNewContext(appJs, sandbox, { filename: 'docs/ui/app.js' });

const song = {
  format: 'KANTANPlayCore',
  type: 'Song',
  version: 3,
  tempo: 120,
  base_key: 0,
  progression: {
    version: 1,
    length: 1,
    timeline: {
      0: { main: '1', slot: 0, part: [0] },
    },
  },
  drum_note: [[57, 42, 46, 50, 39, 38, 36]],
  slot: [{
    chord_mode: {
      part: [{
        tone: 1,
        loop_step: 0,
        arpeggio: [[100]],
      }],
    },
  }],
};

const kantan = {
  getMidiNoteNumber(pitch, degree, key) {
    return 60 + key + degree + pitch;
  },
};

const smf = sandbox.KANPLAY_FILE_UI_TEST.songToSmfWithKantan(song, kantan);
const text = Buffer.from(smf).toString('latin1');
if (!text.startsWith('MThd')) throw new Error('missing MThd header');
if (!text.includes('MTrk')) throw new Error('missing MTrk chunk');
if (sandbox.KANPLAY_FILE_UI_TEST.subStepTick(0, 1, 2, 480, 100) !== 320) {
  throw new Error('unexpected swing tick');
}
console.log(`smf_ok ${smf.length} bytes`);
