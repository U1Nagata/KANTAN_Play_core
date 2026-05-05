(() => {
  const API = (window.KANPLAY && window.KANPLAY.api) || location.origin;
  const UI_BASE = new URL('.', document.currentScript ? document.currentScript.src : location.href).href;
  const DIRS = [
    { token: 'songs/user',       label: 'Songs (user)' },
    { token: 'songs/extra',      label: 'Songs (extra)' },
    { token: 'arpeggio/user',    label: 'Arpeggio (user)' },
    { token: 'progression/user', label: 'Progression (user)' },
  ];

  const root = document.getElementById('app');
  if (!root) return;

  let currentDir = DIRS[0].token;
  let currentFiles = [];
  let selected = new Set();
  let kantanMusicPromise = null;

  function el(tag, attrs, ...children) {
    const e = document.createElement(tag);
    if (attrs) {
      for (const k in attrs) {
        if (k === 'class') e.className = attrs[k];
        else if (k.startsWith('on')) e.addEventListener(k.slice(2), attrs[k]);
        else if (typeof attrs[k] === 'boolean') {
          if (attrs[k]) e.setAttribute(k, k);
        }
        else e.setAttribute(k, attrs[k]);
      }
    }
    for (const c of children) {
      if (c == null || c === false) continue;
      e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
    }
    return e;
  }

  async function api(method, path, body) {
    const opts = { method };
    if (body !== undefined) {
      opts.headers = { 'Content-Type': 'application/json' };
      opts.body = body;
    }
    const r = await fetch(API + path, opts);
    if (!r.ok) {
      let msg = r.status + ' ' + r.statusText;
      try {
        const j = await r.json();
        if (j && j.error) msg += ': ' + j.error;
      } catch (_) { /* not json */ }
      throw new Error(msg);
    }
    const ct = r.headers.get('content-type') || '';
    if (ct.includes('application/json')) return r.json();
    return r.text();
  }

  function updateBulkButtons() {
    const btnDel = document.getElementById('bulk-delete');
    const btnZip = document.getElementById('bulk-zip');
    const chkAll = document.getElementById('chk-all');
    if (!btnDel || !btnZip || !chkAll) return;
    const n = selected.size;
    const disabled = n === 0;
    btnDel.disabled = disabled;
    btnZip.disabled = disabled;
    btnDel.textContent = n > 0 ? 'Delete (' + n + ')' : 'Delete';
    btnZip.textContent = n > 0 ? 'Download ZIP (' + n + ')' : 'Download ZIP';
    chkAll.indeterminate = n > 0 && n < currentFiles.length;
    chkAll.checked = n > 0 && n === currentFiles.length;
  }

  function render() {
    root.innerHTML = '';
    const tabs = el('div', { class: 'tabs' },
      ...DIRS.map(d =>
        el('button', {
          class: 'tab' + (d.token === currentDir ? ' active' : ''),
          onclick: () => { currentDir = d.token; selected.clear(); render(); },
        }, d.label)
      )
    );

    const chkAll = el('input', { type: 'checkbox', id: 'chk-all' });
    chkAll.addEventListener('change', () => {
      if (chkAll.checked) {
        currentFiles.forEach(f => selected.add(f.name));
      } else {
        selected.clear();
      }
      document.querySelectorAll('.item-chk').forEach(c => {
        c.checked = selected.has(c.dataset.name);
      });
      updateBulkButtons();
    });

    const bulkBar = el('div', { class: 'bulk-bar' },
      el('label', { class: 'chk-all-label' },
        chkAll,
        el('span', null, 'Select all')
      ),
      el('button', {
        id: 'bulk-delete',
        class: 'danger',
        onclick: doBulkDelete,
      }, 'Delete'),
      el('button', {
        id: 'bulk-zip',
        onclick: doBulkZip,
      }, 'Download ZIP')
    );

    root.appendChild(el('div', { class: 'wrap' },
      el('h1', null, 'KANTAN Play - Files'),
      tabs,
      bulkBar,
      el('ul', { class: 'list', id: 'list' },
        el('li', { class: 'loading' }, 'Loading…')
      ),
      el('div', { class: 'upload' },
        el('input', { type: 'file', id: 'file', accept: '.json' }),
        el('button', { onclick: doUpload }, 'Upload')
      ),
      el('div', { class: 'license-note' }, 'Compliant with KANTAN Music'),
      el('div', { id: 'status', class: 'status' })
    ));
    updateBulkButtons();
    refresh();
  }

  function setStatus(msg, kind) {
    const s = document.getElementById('status');
    if (!s) return;
    s.textContent = msg || '';
    s.className = 'status' + (kind ? ' ' + kind : '');
  }

  async function refresh() {
    selected.clear();
    const list = document.getElementById('list');
    list.innerHTML = '';
    list.appendChild(el('li', { class: 'loading' }, 'Loading…'));
    try {
      const data = await api('GET', '/api/files/' + currentDir);
      currentFiles = data.files || [];
      list.innerHTML = '';
      if (currentFiles.length === 0) {
        list.appendChild(el('li', { class: 'empty' }, '(empty)'));
        updateBulkButtons();
        return;
      }
      for (const f of currentFiles) {
        list.appendChild(buildItem(f));
      }
      setStatus('');
    } catch (e) {
      currentFiles = [];
      list.innerHTML = '';
      list.appendChild(el('li', { class: 'list-error' }, 'Error: ' + e.message));
    }
    updateBulkButtons();
  }

  function buildItem(f) {
    const li = el('li', { class: 'item' });

    const chk = el('input', { type: 'checkbox', class: 'item-chk' });
    chk.dataset.name = f.name;
    chk.checked = selected.has(f.name);
    chk.addEventListener('change', () => {
      if (chk.checked) selected.add(f.name); else selected.delete(f.name);
      updateBulkButtons();
    });

    const nameSpan = el('span', { class: 'name' }, f.name);
    const sizeSpan = el('span', { class: 'size' }, f.size + ' B');

    const btnDownload = el('button', { onclick: () => doDownloadJson(f.name) }, 'JSON');
    const btnSmf      = el('button', {
      disabled: !currentDir.startsWith('songs/'),
      onclick: () => doDownloadSmf(f.name),
    }, 'SMF');
    const btnRename   = el('button', { onclick: () => startRename() }, 'Rename');
    const btnDelete   = el('button', { class: 'danger', onclick: () => doDelete(f.name) }, 'Delete');

    function startRename() {
      const input = el('input', { class: 'rename-input', value: f.name, type: 'text' });
      const btnOk     = el('button', { class: 'rename-ok', onclick: commitRename }, 'OK');
      const btnCancel = el('button', { onclick: cancelRename }, 'Cancel');

      li.innerHTML = '';
      li.appendChild(input);
      li.appendChild(btnOk);
      li.appendChild(btnCancel);
      input.focus();
      input.select();
      input.addEventListener('keydown', e => {
        if (e.key === 'Enter' && !e.isComposing) commitRename();
        if (e.key === 'Escape') cancelRename();
      });

      async function commitRename() {
        const newName = input.value.trim();
        if (!newName || newName === f.name) { cancelRename(); return; }
        try {
          setStatus('Renaming…');
          await api('POST',
            '/api/files/' + currentDir + '/' + encodeURIComponent(f.name) + '/rename?to=' + encodeURIComponent(newName)
          );
          setStatus('Renamed to ' + newName, 'ok');
          refresh();
        } catch (e) {
          setStatus('Rename failed: ' + e.message, 'error');
          cancelRename();
        }
      }

      function cancelRename() {
        li.innerHTML = '';
        li.appendChild(chk);
        li.appendChild(nameSpan);
        li.appendChild(sizeSpan);
        li.appendChild(btnDownload);
        li.appendChild(btnSmf);
        li.appendChild(btnRename);
        li.appendChild(btnDelete);
      }
    }

    li.appendChild(chk);
    li.appendChild(nameSpan);
    li.appendChild(sizeSpan);
    li.appendChild(btnDownload);
    li.appendChild(btnSmf);
    li.appendChild(btnRename);
    li.appendChild(btnDelete);
    return li;
  }

  async function doDownloadJson(name) {
    try {
      setStatus('Downloading ' + name + ' …');
      const r = await fetch(API + '/api/files/' + currentDir + '/' + encodeURIComponent(name));
      if (!r.ok) {
        let msg = r.status + ' ' + r.statusText;
        try { const j = await r.json(); if (j && j.error) msg += ': ' + j.error; } catch (_) { /* not json */ }
        throw new Error(msg);
      }
      const blob = await r.blob();
      triggerDownload(blob, name);
      setStatus('Downloaded ' + name, 'ok');
    } catch (e) {
      setStatus('Download failed: ' + e.message, 'error');
    }
  }

  async function doDownloadSmf(name) {
    try {
      setStatus('Creating SMF from ' + name + ' …');
      const song = await fetchJsonFile(name);
      const smf = await songToSmf(song);
      const outName = name.replace(/\.json$/i, '.mid');
      triggerDownload(new Blob([smf], { type: 'audio/midi' }), outName);
      setStatus('Downloaded ' + outName, 'ok');
    } catch (e) {
      setStatus('SMF export failed: ' + e.message, 'error');
    }
  }

  async function fetchJsonFile(name) {
    const r = await fetch(API + '/api/files/' + currentDir + '/' + encodeURIComponent(name));
    if (!r.ok) {
      let msg = r.status + ' ' + r.statusText;
      try { const j = await r.json(); if (j && j.error) msg += ': ' + j.error; } catch (_) { /* not json */ }
      throw new Error(msg);
    }
    return r.json();
  }

  async function doDelete(name) {
    if (!confirm('Delete ' + name + '?')) return;
    try {
      setStatus('Deleting ' + name + ' …');
      await api('DELETE', '/api/files/' + currentDir + '/' + encodeURIComponent(name));
      setStatus('Deleted ' + name, 'ok');
      refresh();
    } catch (e) {
      setStatus('Delete failed: ' + e.message, 'error');
    }
  }

  async function doBulkDelete() {
    const names = Array.from(selected);
    if (names.length === 0) return;
    if (!confirm('Delete ' + names.length + ' file(s)?')) return;
    setStatus('Deleting ' + names.length + ' file(s)…');
    let failed = 0;
    for (const name of names) {
      try {
        await api('DELETE', '/api/files/' + currentDir + '/' + encodeURIComponent(name));
      } catch (_) {
        failed++;
      }
    }
    if (failed === 0) {
      setStatus('Deleted ' + names.length + ' file(s)', 'ok');
    } else {
      setStatus('Deleted ' + (names.length - failed) + ' file(s), ' + failed + ' failed', 'error');
    }
    refresh();
  }

  async function doBulkZip() {
    const names = Array.from(selected);
    if (names.length === 0) return;

    // JSZip をまだ読み込んでいなければ動的に追加する
    if (!window.JSZip) {
      setStatus('Loading ZIP library…');
      await loadScript('https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js');
    }

    setStatus('Downloading ' + names.length + ' file(s)…');
    const zip = new window.JSZip();
    let failed = 0;
    for (let i = 0; i < names.length; i++) {
      const name = names[i];
      setStatus('Fetching ' + (i + 1) + ' / ' + names.length + ': ' + name + '…');
      try {
        const r = await fetch(API + '/api/files/' + currentDir + '/' + encodeURIComponent(name));
        if (!r.ok) throw new Error(r.status);
        const buf = await r.arrayBuffer();
        zip.file(name, buf);
      } catch (_) {
        failed++;
      }
    }

    setStatus('Creating ZIP…');
    const blob = await zip.generateAsync({ type: 'blob' });
    const dirLabel = currentDir.replace('/', '_');
    triggerDownload(blob, 'kanplay_' + dirLabel + '.zip');

    if (failed === 0) {
      setStatus('Downloaded ' + names.length + ' file(s) as ZIP', 'ok');
    } else {
      setStatus('ZIP created. ' + failed + ' file(s) failed to fetch', 'error');
    }
  }

  function triggerDownload(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = el('a', { href: url, download: filename });
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  }

  function loadScript(src) {
    return new Promise((resolve, reject) => {
      const s = document.createElement('script');
      s.src = src;
      s.onload = resolve;
      s.onerror = reject;
      document.head.appendChild(s);
    });
  }

  async function getKantanMusic() {
    if (kantanMusicPromise) return kantanMusicPromise;
    kantanMusicPromise = (async () => {
      if (window.KANTANMusic && typeof window.KANTANMusic.getMidiNoteNumber === 'function') {
        return window.KANTANMusic;
      }
      try {
        await loadScript(UI_BASE + 'kantan-music/kantan-music.js');
      } catch (_) {
        throw new Error('KANTAN Music WASM is not installed: docs/ui/kantan-music/kantan-music.js');
      }
      if (window.KANTANMusic && typeof window.KANTANMusic.getMidiNoteNumber === 'function') {
        return window.KANTANMusic;
      }
      if (typeof window.createKANTANMusicModule === 'function') {
        const mod = await window.createKANTANMusicModule();
        if (typeof mod._kantan_music_get_midi_note_number === 'function') {
          return {
            getMidiNoteNumber(pitch, degree, key, options) {
              return mod._kantan_music_get_midi_note_number(
                pitch,
                degree,
                key,
                options.voicing || 0,
                options.modifier || 0,
                options.semitone_shift || 0,
                options.bass_degree || 0,
                options.bass_semitone_shift || 0,
                options.position || 0,
                options.minor_swap ? 1 : 0
              );
            }
          };
        }
        return {
          getMidiNoteNumber(pitch, degree, key, options) {
            const ptr = mod._malloc(28);
            try {
              mod.setValue(ptr + 0, options.voicing || 0, 'i32');
              mod.setValue(ptr + 4, options.modifier || 0, 'i32');
              mod.setValue(ptr + 8, options.semitone_shift || 0, 'i32');
              mod.setValue(ptr + 12, options.bass_degree || 0, 'i32');
              mod.setValue(ptr + 16, options.bass_semitone_shift || 0, 'i32');
              mod.setValue(ptr + 20, options.position || 0, 'i32');
              mod.setValue(ptr + 24, options.minor_swap ? 1 : 0, 'i8');
              return mod.ccall('KANTANMusic_GetMidiNoteNumber', 'number',
                ['number', 'number', 'number', 'number'], [pitch, degree, key, ptr]);
            } finally {
              mod._free(ptr);
            }
          }
        };
      }
      throw new Error('KANTAN Music WASM adapter is not compatible');
    })();
    return kantanMusicPromise;
  }

  const MODIFIER = {
    'dim': 1,
    'm7-5': 2,
    'sus4': 3,
    '6': 4,
    '7': 5,
    'add9': 7,
    'M7': 8,
    'aug': 9,
    '7sus4': 10,
    'dim7': 11,
  };
  const VOICING = {
    'Close': 0,
    'Guitar': 1,
    'Static': 2,
    'Ukulele': 3,
    'M': 4,
    'M-Penta': 5,
    'Chromatic': 6,
  };
  const DEFAULT_DRUM_NOTES = [57, 42, 46, 50, 39, 38, 36];

  async function songToSmf(song) {
    const kantan = await getKantanMusic();
    return songToSmfWithKantan(song, kantan);
  }

  function songToSmfWithKantan(song, kantan) {
    if (!song || song.type !== 'Song') throw new Error('Selected file is not a Song JSON');
    if (!song.progression || !song.progression.timeline) throw new Error('Song has no progression');

    const ppq = 480;
    const tempo = song.tempo || 120;
    const swing = clamp(song.swing || 0, 0, 100);
    const baseKey = normalizeKey(song.base_key || 0);
    const slots = resolveSlots(song);
    const eventsByTrack = Array.from({ length: 7 }, () => []);
    const setupState = Array.from({ length: 16 }, () => ({ program: null, volume: null, pan: null }));
    const noteState = createNoteState(eventsByTrack);

    const sortedTimeline = Object.keys(song.progression.timeline)
      .map(k => ({ step: Number(k), value: song.progression.timeline[k] }))
      .sort((a, b) => a.step - b.step);
    const length = song.progression.length || 0;
    const songEndTick = length * ppq;

    const partStep = [0, 0, 0, 0, 0, 0];
    const prevEnabled = [true, true, true, true, true, true];
    let timelineIndex = 0;
    let desc = {
      main: parseDegree('1'),
      modifier: 0,
      bass: null,
      slot: 0,
      partEnabled: [true, true, true, true, true, true],
    };

    for (let beat = 0; beat < length; beat++) {
      while (timelineIndex < sortedTimeline.length && sortedTimeline[timelineIndex].step <= beat) {
        const prevDesc = desc;
        const nextDesc = applyTimelineEntry(desc, sortedTimeline[timelineIndex].value);
        applyTimelineNoteOffs(noteState, partStep, prevEnabled, prevDesc, nextDesc, beat * ppq, slots);
        desc = nextDesc;
        timelineIndex++;
      }
      if (!desc.main || !desc.main.degree) continue;
      const slot = slots[desc.slot] || slots[0];
      const stepPerBeat = clamp(slot.step_per_beat || 2, 1, 4);
      const slotKey = normalizeKey(baseKey + (slot.key_offset || 0));

      for (let sub = 0; sub < stepPerBeat; sub++) {
        const tick = subStepTick(beat, sub, stepPerBeat, ppq, swing);
        for (let partIndex = 0; partIndex < 6; partIndex++) {
          const enabled = !!desc.partEnabled[partIndex];
          const part = slot.parts[partIndex];
          if (!part || part.enabled === false || !enabled) {
            partStep[partIndex] = -1;
            prevEnabled[partIndex] = false;
            continue;
          }
          if (!prevEnabled[partIndex] || partStep[partIndex] < 0) partStep[partIndex] = 0;
          renderPartStep(noteState, setupState, partIndex, part, partStep[partIndex], tick, ppq, tempo, desc, slotKey, kantan);
          partStep[partIndex]++;
          if (partStep[partIndex] > (part.loop_step || 1)) partStep[partIndex] = 0;
          prevEnabled[partIndex] = true;
        }
      }
    }
    closeAllNotes(noteState, songEndTick);

    return buildSmf({
      ppq,
      tempo,
      trackEvents: eventsByTrack,
    });
  }

  function renderPartStep(noteState, setupState, partIndex, part, step, tick, ppq, tempo, desc, slotKey, kantan) {
    if (!part.arpeggio || step < 0) return;
    closeExpiredNotes(noteState, tick);
    const track = part.tone === 128 ? noteState.eventsByTrack[6] : noteState.eventsByTrack[partIndex];
    const channel = part.tone === 128 ? 9 : partIndex;
    const program = part.tone === 128 ? 0 : clamp(part.tone || 0, 0, 127);
    const volume = clamp(Math.round((part.volume == null ? 100 : part.volume) * 127 / 100), 0, 127);
    const pan = panToCc(part.pan || 0);
    const setup = setupState[channel];
    if (part.tone !== 128 && setup.program !== program) {
      pushEvent(track, tick, [0xC0 | channel, program]);
      setup.program = program;
    }
    if (setup.volume !== volume) {
      pushEvent(track, tick, [0xB0 | channel, 7, volume]);
      setup.volume = volume;
    }
    if (setup.pan !== pan) {
      pushEvent(track, tick, [0xB0 | channel, 10, pan]);
      setup.pan = pan;
    }

    const style = part.style[step] || '';
    const isMuteStyle = style === 'M';
    const pitchOrder = style === 'U' ? [0, 1, 2, 3, 4, 5, 6] : [6, 5, 4, 3, 2, 1, 0];
    const strokeMs = part.tone === 128 || style === '' ? 0 : (part.stroke_speed || 0);
    const strokeTicks = Math.max(0, msToTicks(isMuteStyle ? strokeMs / 4 : strokeMs, tempo, ppq));
    const defaultReleaseTicks = msToTicks(5000, tempo, ppq);
    const muteReleaseTicks = Math.max(1, msToTicks(strokeMs * 2, tempo, ppq));
    let strokeIndex = 0;

    for (const pitchIndex of pitchOrder) {
      const velocity = getArpVelocity(part.arpeggio, pitchIndex, step);
      if (!velocity && !isMuteStyle) continue;
      const noteTick = tick + strokeIndex * strokeTicks;
      closeExpiredNotes(noteState, noteTick);
      const midiNote = part.tone === 128
        ? part.drum_note[pitchIndex]
        : kantan.getMidiNoteNumber(6 - pitchIndex, desc.main.degree, slotKey, {
            voicing: VOICING[part.voicing || 'Close'] || 0,
            modifier: desc.modifier || 0,
            semitone_shift: desc.main.semitone || 0,
            bass_degree: desc.bass ? desc.bass.degree : 0,
            bass_semitone_shift: desc.bass ? desc.bass.semitone : 0,
            position: part.octave || 0,
            minor_swap: !!desc.main.minorSwap,
          });
      if (midiNote > 0) {
        const releaseTick = noteTick + (isMuteStyle ? muteReleaseTicks : defaultReleaseTicks);
        if (velocity > 0) {
          const vel = clamp(velocity, 1, 127);
          noteOn(noteState, partIndex, pitchIndex, channel, midiNote, vel, noteTick, releaseTick);
        } else {
          noteOffPartPitch(noteState, partIndex, pitchIndex, noteTick);
          noteOffChannelNote(noteState, channel, midiNote, noteTick);
        }
      }
      strokeIndex++;
    }
  }

  function applyTimelineNoteOffs(noteState, partStep, prevEnabled, prevDesc, nextDesc, tick, slots) {
    const chordChanged = !sameDegree(prevDesc.main, nextDesc.main)
      || !sameDegree(prevDesc.bass, nextDesc.bass)
      || prevDesc.modifier !== nextDesc.modifier
      || prevDesc.slot !== nextDesc.slot;
    for (let partIndex = 0; partIndex < 6; partIndex++) {
      const prevPartEnabled = !!prevDesc.partEnabled[partIndex];
      const nextPartEnabled = !!nextDesc.partEnabled[partIndex];
      const slot = slots[nextDesc.slot] || slots[0];
      const nextPart = slot && slot.parts[partIndex];
      const nextEnabled = nextPart && nextPart.enabled !== false && nextPartEnabled;
      if (chordChanged || prevPartEnabled !== nextPartEnabled || !nextEnabled) {
        noteOffPart(noteState, partIndex, tick);
        if (!nextEnabled) {
          partStep[partIndex] = -1;
          prevEnabled[partIndex] = false;
        }
      }
    }
  }

  function createNoteState(eventsByTrack) {
    return { eventsByTrack, active: [] };
  }

  function noteOn(state, part, pitch, channel, note, velocity, tick, autoOffTick) {
    noteOffPartPitch(state, part, pitch, tick);
    noteOffChannelNote(state, channel, note, tick);
    const trackIndex = channel === 9 ? 6 : part;
    pushEvent(state.eventsByTrack[trackIndex], tick, [0x90 | channel, note, velocity]);
    state.active.push({ part, pitch, channel, note, trackIndex, autoOffTick });
  }

  function noteOffPartPitch(state, part, pitch, tick) {
    closeMatchingNotes(state, tick, n => n.part === part && n.pitch === pitch);
  }

  function noteOffPart(state, part, tick) {
    closeMatchingNotes(state, tick, n => n.part === part);
  }

  function noteOffChannelNote(state, channel, note, tick) {
    closeMatchingNotes(state, tick, n => n.channel === channel && n.note === note);
  }

  function closeExpiredNotes(state, tick) {
    closeMatchingNotes(state, tick, n => n.autoOffTick <= tick);
  }

  function closeAllNotes(state, tick) {
    closeMatchingNotes(state, tick, () => true);
  }

  function closeMatchingNotes(state, tick, predicate) {
    const keep = [];
    for (const note of state.active) {
      if (predicate(note)) {
        const offTick = Math.max(0, Math.min(tick, note.autoOffTick));
        pushEvent(state.eventsByTrack[note.trackIndex], offTick, [0x80 | note.channel, note.note, 0]);
      } else {
        keep.push(note);
      }
    }
    state.active = keep;
  }

  function resolveSlots(song) {
    const rawSlots = song.slot || [];
    const result = [];
    for (let i = 0; i < Math.max(rawSlots.length, 1); i++) {
      result[i] = resolveSlot(i, rawSlots, result, song.drum_note || []);
    }
    return result;
  }

  function resolveSlot(index, rawSlots, resolved, topDrums) {
    if (resolved[index]) return resolved[index];
    const raw = rawSlots[index] || {};
    if (raw.copy && raw.copy.length) {
      const base = clone(resolveSlot(raw.copy[0], rawSlots, resolved, topDrums));
      resolved[index] = base;
      return base;
    }
    const slot = {
      key_offset: raw.key_offset || 0,
      step_per_beat: raw.step_per_beat || 2,
      parts: [],
    };
    const rawParts = raw.chord_mode && raw.chord_mode.part || [];
    for (let partIndex = 0; partIndex < 6; partIndex++) {
      slot.parts[partIndex] = resolvePart(index, partIndex, rawParts, rawSlots, resolved, topDrums);
    }
    resolved[index] = slot;
    return slot;
  }

  function resolvePart(slotIndex, partIndex, rawParts, rawSlots, resolvedSlots, topDrums) {
    const raw = rawParts[partIndex] || {};
    if (raw.copy && raw.copy.length >= 2) {
      return clone(resolveSlot(raw.copy[0], rawSlots, resolvedSlots, topDrums).parts[raw.copy[1]]);
    }
    const base = {
      tone: 0,
      volume: 100,
      octave: 0,
      voicing: 'Close',
      loop_step: 1,
      anchor_step: 0,
      stroke_speed: 20,
      enabled: true,
      pan: 0,
      arpeggio: [],
      style: [],
      drum_note: topDrums[partIndex] || DEFAULT_DRUM_NOTES,
    };
    return Object.assign(base, raw, {
      arpeggio: raw.arpeggio || base.arpeggio,
      style: raw.style || base.style,
      drum_note: raw.drum_note || base.drum_note,
    });
  }

  function applyTimelineEntry(prev, entry) {
    const next = {
      main: entry.main ? parseDegree(entry.main) : prev.main,
      modifier: entry.mod ? (MODIFIER[entry.mod] || 0) : 0,
      bass: entry.bass ? parseDegree(entry.bass) : null,
      slot: Number.isInteger(entry.slot) ? entry.slot : prev.slot,
      partEnabled: prev.partEnabled.slice(),
    };
    if (Array.isArray(entry.part)) {
      next.partEnabled = [false, false, false, false, false, false];
      for (const p of entry.part) {
        if (p >= 0 && p < 6) next.partEnabled[p] = true;
      }
    }
    return next;
  }

  function parseDegree(text) {
    const s = String(text || '');
    const m = s.match(/^([1-7])([b#]?)(~?)$/);
    if (!m) return { degree: 0, semitone: 0, minorSwap: false };
    return {
      degree: Number(m[1]),
      semitone: m[2] === 'b' ? -1 : (m[2] === '#' ? 1 : 0),
      minorSwap: m[3] === '~',
    };
  }

  function sameDegree(a, b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    return a.degree === b.degree
      && a.semitone === b.semitone
      && a.minorSwap === b.minorSwap;
  }

  function getArpVelocity(arpeggio, pitch, step) {
    const row = arpeggio[pitch];
    if (!Array.isArray(row) || step >= row.length) return 0;
    return Number(row[step]) || 0;
  }

  function subStepTick(beat, sub, stepPerBeat, ppq, swing) {
    const beatTick = beat * ppq;
    const stepTick = ppq / stepPerBeat;
    if ((stepPerBeat & 1) !== 0 || swing <= 0) {
      return Math.round(beatTick + sub * stepTick);
    }
    const swingX100 = swing * 100 / 3;
    const longTick = stepTick + (stepTick * swingX100 / 10000);
    const shortTick = stepTick * 2 - longTick;
    let offset = 0;
    for (let i = 0; i < sub; i++) {
      offset += (i & 1) === 0 ? longTick : shortTick;
    }
    return Math.round(beatTick + offset);
  }

  function msToTicks(ms, tempo, ppq) {
    return Math.max(0, Math.round(ms * tempo * ppq / 60000));
  }

  function buildSmf({ ppq, tempo, trackEvents }) {
    const tracks = [];
    const tempoUs = Math.round(60000000 / tempo);
    tracks.push(trackChunk([
      { tick: 0, data: [0xFF, 0x51, 0x03, (tempoUs >> 16) & 255, (tempoUs >> 8) & 255, tempoUs & 255] },
      { tick: 0, data: [0xFF, 0x58, 0x04, 4, 2, 24, 8] },
    ]));
    for (const events of trackEvents) {
      tracks.push(trackChunk(events));
    }
    return concatBytes([
      ascii('MThd'), u32(6), u16(1), u16(tracks.length), u16(ppq),
      ...tracks,
    ]);
  }

  function trackChunk(events) {
    const sorted = events.slice().sort((a, b) => a.tick - b.tick);
    const body = [];
    let lastTick = 0;
    for (const ev of sorted) {
      body.push(...vlq(Math.max(0, ev.tick - lastTick)), ...ev.data);
      lastTick = ev.tick;
    }
    body.push(0, 0xFF, 0x2F, 0);
    return concatBytes([ascii('MTrk'), u32(body.length), body]);
  }

  function pushEvent(events, tick, data) {
    events.push({ tick: Math.max(0, Math.round(tick)), data });
  }

  function vlq(value) {
    let buffer = value & 0x7F;
    const out = [];
    while ((value >>= 7)) {
      buffer <<= 8;
      buffer |= ((value & 0x7F) | 0x80);
    }
    for (;;) {
      out.push(buffer & 0xFF);
      if (buffer & 0x80) buffer >>= 8;
      else break;
    }
    return out;
  }

  function ascii(s) { return Array.from(s).map(c => c.charCodeAt(0)); }
  function u16(v) { return [(v >> 8) & 255, v & 255]; }
  function u32(v) { return [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]; }
  function concatBytes(chunks) {
    const size = chunks.reduce((n, c) => n + c.length, 0);
    const out = new Uint8Array(size);
    let offset = 0;
    for (const c of chunks) {
      out.set(c, offset);
      offset += c.length;
    }
    return out;
  }
  function clamp(v, min, max) { return Math.min(max, Math.max(min, Number(v) || 0)); }
  function normalizeKey(v) { v %= 12; return v < 0 ? v + 12 : v; }
  function panToCc(pan) { return [0, 13, 26, 39, 52, 64, 76, 89, 102, 115, 127][clamp(pan + 5, 0, 10)]; }
  function clone(value) { return JSON.parse(JSON.stringify(value)); }

  async function doUpload() {
    const input = document.getElementById('file');
    const f = input && input.files[0];
    if (!f) { setStatus('Please select a file', 'error'); return; }
    if (!f.name.toLowerCase().endsWith('.json')) {
      setStatus('Only .json files can be uploaded', 'error');
      return;
    }
    try {
      const text = await f.text();
      setStatus('Uploading ' + f.name + ' …');
      await api('PUT',
        '/api/files/' + currentDir + '/' + encodeURIComponent(f.name),
        text
      );
      setStatus('Uploaded ' + f.name, 'ok');
      input.value = '';
      refresh();
    } catch (e) {
      setStatus('Upload failed: ' + e.message, 'error');
    }
  }

  window.KANPLAY_FILE_UI_TEST = {
    songToSmfWithKantan,
    buildSmf,
    parseDegree,
    subStepTick,
  };

  render();
})();
