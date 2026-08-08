(() => {
  const PREVIEW = !(window.KANPLAY && window.KANPLAY.api) || new URLSearchParams(location.search).has('demo');
  const API = PREVIEW ? '' : window.KANPLAY.api;
  const $ = (s, root = document) => root.querySelector(s);
  const el = (tag, attrs = {}, ...children) => {
    const node = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') node.className = v;
      else if (k.startsWith('on')) node.addEventListener(k.slice(2), v);
      else if (v !== undefined && v !== null) node.setAttribute(k, v);
    }
    node.append(...children.flat().filter(v => v !== null && v !== undefined && v !== false)
      .map(v => typeof v === 'string' ? document.createTextNode(v) : v));
    return node;
  };
  let state = null;
  let selectedPad = 0;
  let files = { samples: [], loops: [], kits: [], projects: [] };
  let folders = { samples: [], loops: [], kits: [], projects: [] };
  let browseFolders = { samples:null, loops:null, kits:null, projects:null };
  let loopEventsDraft = null;

  function previewWave(seed) {
    return Array.from({ length:96 }, (_, i) => {
      const envelope = Math.max(0.08, 1 - i / 110);
      const a = Math.sin((i + seed * 7) * (0.31 + seed * 0.013)) * envelope;
      const b = Math.sin((i + seed * 11) * (0.57 + seed * 0.009)) * envelope * .6;
      const peak = Math.round(Math.min(1, Math.abs(a + b)) * 27000);
      return [-peak, peak];
    });
  }
  function previewPad(index, name) {
    const active = Boolean(name);
    const frames = active ? 48000 : 0;
    return { pad:index, label:index + 1, name:name || '', frames, sampleRate:48000,
      start:active ? 160 : 0, end:active ? frames - 480 : 0, volume:256, pitch:256,
      reverse:false, hold:false, loop:false, wave:active ? previewWave(index + 1) : [] };
  }
  function createPreviewState() {
    const names = ['KICK 808', 'SNARE', 'CLAP', 'HAT', 'PIKO', 'COWBELL', 'CHIN', 'TOM', '', '', '', ''];
    return {
      pads:names.map((name, index) => previewPad(index, name)),
      builtinSamples:names.slice(0,8).map((name, index) => ({
        name,file:'builtin:'+name,
        category:['Kick','Snare','Percussion','HiHat','FX','Percussion','Cymbal','Tom'][index]
      })),
      builtinBackgrounds:[{name:'HOUSE AUDIO',file:'builtin:BGM_House.wav'}],
      builtinBeatPatterns:[
        {name:'POP',file:'pattern:POP'},{name:'ROCK',file:'pattern:ROCK'},
        {name:'HOUSE',file:'pattern:HOUSE'},{name:'HIP HOP',file:'pattern:HIP HOP'},
        {name:'DISCO',file:'pattern:DISCO'},{name:'BREAK',file:'pattern:BREAK'}
      ],
      beat:{format:'audio',name:'HOUSE AUDIO',volume:100},
      loop:{ lengthMs:4000, lengthFixed:true, quantize:true, noteGridIndex:4, noteOffGridIndex:4,
        background:{ file:'/sampler/loops/BGM_House.wav', name:'BGM_HOUSE', frames:192000, sampleRate:48000, volume:208 },
        events:[
          {pad:0,pos:0,type:'on',layer:0}, {pad:3,pos:500,type:'on',layer:0},
          {pad:1,pos:1000,type:'on',layer:0}, {pad:3,pos:1500,type:'on',layer:0},
          {pad:0,pos:2000,type:'on',layer:0}, {pad:4,pos:2250,type:'on',layer:0},
          {pad:1,pos:3000,type:'on',layer:0}, {pad:3,pos:3500,type:'on',layer:0}
        ] },
      folders:{ samples:'/sampler/samples', loops:'/sampler/loops', kits:'/sampler/kits', projects:'/sampler/projects' },
      project:{file:''}, commandRevision:0
    };
  }
  const previewState = createPreviewState();
  const previewFiles = {
    samples:[
      {name:'kick.wav',size:20032}, {name:'Snare.wav',size:44482},
      {name:'clap.wav',size:8620}, {name:'Hat.wav',size:80896}, {name:'Piko.wav',size:19694},
      {name:'Cowbell.wav',size:46158}, {name:'chin.wav',size:28422}, {name:'Tom.wav',size:37764}
    ],
    loops:[{name:'BGM_House.wav',size:192046}, {name:'night-drive.wav',size:704000}],
    kits:[{name:'Starter Beat.json',size:2148}, {name:'Pentatonic Jam.json',size:2331}],
    projects:[{name:'First Jam.json',size:8420}, {name:'Night Session.json',size:9172}]
  };
  const previewFolders = { samples:['Drums', 'Synth'], loops:['Practice'], kits:['Favorites'], projects:['Ideas','Live Sets'] };

  function rootFolder(kind) { return '/sampler/' + kind; }
  function audioPath(kind, value) {
    return value && (value.startsWith('builtin:') || value.startsWith('pattern:'))
      ? value : state.folders[kind] + '/' + value;
  }
  function builtinFiles(kind) {
    const source = kind === 'samples' ? state.builtinSamples
      : kind === 'loops' ? [...(state.builtinBeatPatterns || []), ...(state.builtinBackgrounds || [])] : [];
    return (source || []).map(item => ({...item, builtin:true}));
  }
  function relativeFolder(kind, full = state && state.folders && state.folders[kind]) {
    const root = rootFolder(kind);
    return full && full.startsWith(root + '/') ? full.slice(root.length + 1) : '';
  }
  function activeFolder(kind) {
    return browseFolders[kind] === null ? relativeFolder(kind) : browseFolders[kind];
  }
  function browserFilePath(kind, name) {
    const relative = activeFolder(kind);
    return rootFolder(kind) + '/' + (relative ? relative + '/' : '') + name;
  }
  async function listFiles(kind) {
    const path = activeFolder(kind);
    return request('/api/sampler/files/' + kind + (path ? '?path=' + encodeURIComponent(path) : '')).then(r => r.json());
  }
  async function listFolders(kind, relative = activeFolder(kind)) {
    const path = relative ? '?path=' + encodeURIComponent(relative) : '';
    return request('/api/sampler/folders/' + kind + path).then(r => r.json());
  }

  async function request(path, options = {}) {
    const res = await fetch(API + path, options);
    if (!res.ok) { let msg = res.statusText; try { msg = (await res.json()).error || msg; } catch (_) {} throw new Error(msg); }
    return res;
  }
  function status(text, error = false) { const n = $('#status'); n.textContent = text; n.style.color = error ? 'var(--danger)' : ''; }
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  async function waitForCommandApplied(payload) {
    if (['saveProject','loadProject','newProject','projectRenamed'].includes(payload.action)) {
      const revision = Number(state && state.commandRevision || 0);
      const until = Date.now() + 20000;
      while (Date.now() < until) {
        const next = await request('/api/sampler/state').then(r => r.json());
        if (Number(next.commandRevision || 0) !== revision) { state = next; return true; }
        await sleep(150);
      }
      throw new Error('project operation timed out');
    }
    if (!['assignSample', 'clearPad'].includes(payload.action)) {
      await sleep(180);
      return false;
    }
    const until = Date.now() + 4000;
    const matches = next => {
      const pad = next.pads && next.pads.find(p => p.pad === payload.pad);
      if (payload.action === 'assignSample') {
        return pad && pad.frames > 0 && (pad.file === payload.file || pad.name === previewFileName(payload.file));
      }
      if (payload.action === 'clearPad') return pad && !pad.frames;
      return true;
    };
    while (Date.now() < until) {
      const next = await request('/api/sampler/state').then(r => r.json());
      if (matches(next)) { state = next; return true; }
      await sleep(120);
    }
    return false;
  }
  async function command(payload, refreshAfter = true) {
    if (PREVIEW) {
      applyPreviewCommand(payload);
      loopEventsDraft = null;
      if (refreshAfter) render();
      status('Preview mode');
      return;
    }
    status('Applying…');
    await request('/api/sampler/command', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload) });
    if (!refreshAfter) {
      status(payload.action === 'previewWav' ? 'Previewing…' : 'Playing…');
      return;
    }
    await waitForCommandApplied(payload);
    await refresh();
  }
  async function refresh() {
    if (PREVIEW) {
      state = previewState;
      files = previewFiles;
      folders = previewFolders;
      loopEventsDraft = null;
      if (!state.pads.some(p => p.pad === selectedPad)) selectedPad = 0;
      render(); status('Preview mode');
      return;
    }
    try {
      state = await request('/api/sampler/state').then(r => r.json());
      const projectApi = Boolean(state.folders && state.folders.projects && $('#project-view'));
      const results = await Promise.allSettled([
        listFiles('samples'), listFiles('loops'), listFiles('kits'),
        projectApi ? listFiles('projects') : Promise.resolve({files:[]}),
        listFolders('samples'), listFolders('loops'), listFolders('kits'),
        projectApi ? listFolders('projects') : Promise.resolve({folders:[]})
      ]);
      const value = (index, fallback) => results[index].status === 'fulfilled' ? results[index].value : fallback;
      files = {
        samples:value(0,{files:[]}).files || [],
        loops:value(1,{files:[]}).files || [],
        kits:value(2,{files:[]}).files || [],
        projects:value(3,{files:[]}).files || []
      };
      folders = {
        samples:value(4,{folders:[]}).folders || [],
        loops:value(5,{folders:[]}).folders || [],
        kits:value(6,{folders:[]}).folders || [],
        projects:value(7,{folders:[]}).folders || []
      };
      loopEventsDraft = null;
      if (!state.pads.some(p => p.pad === selectedPad)) selectedPad = 0;
      render(); status(results.some(result => result.status === 'rejected') ? 'Connected / SD unavailable' : 'Connected');
    } catch (err) { status('Connection error: ' + err.message, true); }
  }
  function previewFileName(path) { return (path || '').split('/').pop().replace(/\.(wav|mp3)$/i, ''); }
  function applyPreviewCommand(payload) {
    const pad = previewState.pads.find(p => p.pad === payload.pad);
    if (payload.action === 'setPad' && pad) Object.assign(pad, payload);
    if (payload.action === 'clearPad' && pad) Object.assign(pad, previewPad(pad.pad, ''));
    if (payload.action === 'assignSample' && pad) {
      Object.assign(pad, previewPad(pad.pad, previewFileName(payload.file)));
    }
    if (payload.action === 'loadBeat' || payload.action === 'loadBgm') {
      if (String(payload.file).startsWith('pattern:')) {
        previewState.beat = {format:'pattern',name:String(payload.file).slice(8),volume:previewState.beat.volume};
        previewState.loop.background = { file:'', name:'', frames:0, sampleRate:48000, volume:208 };
        return;
      }
      const name = previewFileName(payload.file);
      previewState.beat = {format:'audio',name,volume:previewState.beat.volume};
      previewState.loop.background = { file:payload.file, name, frames:192000, sampleRate:48000, volume:208 };
    }
    if (payload.action === 'newBeatPattern') previewState.beat = {format:'pattern',name:'NEW PATTERN',volume:previewState.beat.volume};
    if (payload.action === 'clearBeat' || payload.action === 'clearBgm') {
      previewState.beat = {format:'none',name:'',volume:previewState.beat.volume};
      previewState.loop.background = { file:'', name:'', frames:0, sampleRate:48000, volume:208 };
    }
    if (payload.action === 'setLoop') {
      const patch = {...payload}; delete patch.action;
      if (patch.backgroundVolume !== undefined) {
        previewState.loop.background.volume = patch.backgroundVolume;
        delete patch.backgroundVolume;
      }
      if (patch.beatVolume !== undefined) {
        previewState.beat.volume = patch.beatVolume;
        delete patch.beatVolume;
      }
      Object.assign(previewState.loop, patch);
    }
    if (payload.action === 'setEvents') previewState.loop.events = payload.events.map(event => ({...event}));
    if (payload.action === 'saveKit') {
      const name = payload.file.split('/').pop();
      if (!previewFiles.kits.some(file => file.name === name)) previewFiles.kits.push({name, size:2200});
    }
    if (payload.action === 'saveProject') {
      const name = payload.file.split('/').pop();
      if (!previewFiles.projects.some(file => file.name === name)) previewFiles.projects.push({name,size:8600});
      previewState.project.file = payload.file;
    }
    if (payload.action === 'loadProject') previewState.project.file = payload.file;
    if (payload.action === 'projectRenamed' && previewState.project.file === payload.old) previewState.project.file = payload.file;
    if (payload.action === 'newProject') previewState.project.file = '';
    previewState.commandRevision++;
    if (payload.action === 'setFolder' && previewState.folders[payload.kind] !== undefined) {
      previewState.folders[payload.kind] = payload.path;
    }
  }
  function waveSvg(pad) {
    const points = pad.wave || [];
    if (!points.length) return el('svg', { class:'wave', viewBox:'0 0 96 30' });
    const lines = points.map((pair, i) => {
      const y1 = 15 - (pair[1] / 32768) * 14;
      const y2 = 15 - (pair[0] / 32768) * 14;
      return el('line', { x1:i + .5, x2:i + .5, y1, y2, stroke:'#8ee7ff', 'stroke-width':1 });
    });
    return el('svg', { class:'wave', viewBox:'0 0 96 30', preserveAspectRatio:'none' }, lines);
  }
  function parameterValueText(key, value) {
    if (key === 'beatPercent') return Number(value) + '%';
    return /volume|pitch/i.test(key) ? Math.round(Number(value) * 100 / 256) + '%' : String(value);
  }
  function rangeRow(label, key, value, max, onChange) {
    const input = el('input', { type:'range', min:0, max, value });
    const out = el('span', { class:'range-value' }, parameterValueText(key, value));
    input.addEventListener('input', () => { out.textContent = parameterValueText(key, input.value); });
    input.addEventListener('change', () => onChange(Number(input.value)));
    return el('div', { class:'row' }, el('label', {}, label), input, out);
  }
  function padCard(pad) {
    const button = el('button', { class:'pad' + (pad.pad === selectedPad ? ' selected' : '') + (!pad.frames ? ' empty' : ''), onclick:async() => {
      selectedPad = pad.pad;
      renderSamples();
      if (pad.frames) await command({action:'playPad',pad:pad.pad}, false);
    } },
      el('strong', {}, 'P' + pad.label), el('small', {}, pad.name || 'Empty'), waveSvg(pad));
    return button;
  }
  function fileDisplayLabel(kind, file) {
    const name = String(file.name || file.file || '').replace(/\.[^.]+$/, '');
    if (file.builtin) return name;
    const folder = activeFolder(kind) || kind;
    return 'SD/' + folder + '/' + name;
  }
  function optionList(kind, items, selected, empty = 'Select file') {
    const option = f => {
      const value = f.file || f.name;
      const label = fileDisplayLabel(kind, f);
      const active = value === selected || f.name === selected || String(selected).endsWith('/' + f.name);
      return el('option', { value, selected:active ? '' : null }, label);
    };
    if (kind !== 'samples') return [el('option', { value:'' }, empty), ...items.map(option)];

    const result = [el('option', { value:'' }, empty)];
    const categories = new Map();
    for (const item of items.filter(item => item.builtin)) {
      const category = item.category || 'Other';
      if (!categories.has(category)) categories.set(category, []);
      categories.get(category).push(option(item));
    }
    for (const [category, options] of categories) {
      result.push(el('optgroup', { label:category }, options));
    }
    const sd = items.filter(item => !item.builtin);
    if (sd.length) result.push(el('optgroup', { label:'SD Card' }, sd.map(option)));
    return result;
  }
  function renderSamples() {
    const root = $('#sample-view'); root.innerHTML = '';
    // 本体と同じ内部Pad順: 上段 P9-P12 / 中段 P5-P8 / 下段 P1-P4。
    const displayPads = [...state.pads].sort((a, b) => a.pad - b.pad);
    const grid = el('div', { class:'panel' }, el('h2', {}, 'Pads'), el('div', { class:'pad-grid' }, displayPads.map(padCard)));
    const pad = state.pads.find(p => p.pad === selectedPad) || state.pads[0];
    const edit = el('div', { class:'panel' }, el('h2', {}, 'Pad ' + pad.label + '  ' + (pad.name || 'Empty')));
    const sampleSelect = el('select', {}, optionList('samples', [...builtinFiles('samples'), ...files.samples], pad.file || ''));
    edit.append(el('div', { class:'row' }, el('label', {}, 'Audio file'), sampleSelect));
    edit.append(el('div', { class:'actions' },
      el('button', { class:'primary', onclick:async() => { if (sampleSelect.value) await command({action:'assignSample',pad:pad.pad,file:audioPath('samples',sampleSelect.value)}); } }, 'Assign'),
      el('button', { class:'danger', onclick:async() => await command({action:'clearPad',pad:pad.pad}) }, 'Clear'),
      el('button', { onclick:async() => await command(sampleSelect.value
        ? {action:'previewWav',file:audioPath('samples',sampleSelect.value)}
        : {action:'playPad',pad:pad.pad}, false) }, 'Play'),
      el('button', { onclick:async() => await command({action:'stopAudio'}, false) }, 'Stop')));
    if (pad.frames) {
      const apply = async patch => command({ action:'setPad', pad:pad.pad, ...patch });
      edit.append(waveSvg(pad));
      const details = el('details', { class:'detail-settings' }, el('summary', {}, 'Detail settings'));
      details.append(rangeRow('Start', 'start', pad.start, Math.max(1, pad.frames - 1), v => apply({start:v})));
      details.append(rangeRow('End', 'end', pad.end, pad.frames, v => apply({end:v})));
      details.append(rangeRow('Volume', 'volume', pad.volume, 512, v => apply({volume:v})));
      details.append(rangeRow('Pitch', 'pitch', pad.pitch, 512, v => apply({pitch:Math.max(128,v)})));
      for (const [key, label] of [['reverse','Reverse'],['hold','Hold'],['loop','Loop']]) {
        const check = el('input', { type:'checkbox' }); check.checked = !!pad[key];
        check.addEventListener('change', () => apply({[key]:check.checked}));
        details.append(el('div', { class:'row' }, el('label', {}, label), check));
      }
      edit.append(details);
    } else edit.append(el('p', { class:'hint' }, 'Upload or select a WAV/MP3 file, then assign it to this pad.'));
    const library = el('div', { class:'panel' }, el('h2', {}, 'Sample files'), folderPanel('samples'), filePanel('samples', '.wav,.mp3'));
    root.append(el('div', { class:'grid' }, grid, edit), library);
  }
  function renderLoop() {
    const root = $('#loop-view'); root.innerHTML = '';
    const loop = state.loop;
    const beatState = state.beat || {format:loop.background && loop.background.frames ? 'audio' : 'none',name:loop.background && loop.background.name || '',volume:100};
    const bgm = el('div', { class:'panel' }, el('h2', {}, 'Beat'));
    const currentBeatFile = beatState.format === 'pattern'
      ? 'pattern:' + String(beatState.name || '').replace(/ PATTERN$/i, '')
      : loop.background.file || '';
    const select = el('select', {}, optionList('loops', [...builtinFiles('loops'), ...files.loops], currentBeatFile));
    bgm.append(el('div', { class:'row' }, el('label', {}, 'Audio or pattern'), select));
    bgm.append(el('div', { class:'actions' },
      el('button', {class:'primary', onclick:async() => { if (select.value) await command({action:'loadBeat',file:audioPath('loops',select.value)}); }}, 'Load Beat'),
      el('button', {onclick:async()=>await command({action:'newBeatPattern'})}, 'New Pattern'),
      el('button', {class:'danger',onclick:async()=>await command({action:'clearBeat'})}, 'Clear Beat'),
      el('button', {onclick:async()=>await command({action:'playBgm'}, false),disabled:beatState.format!=='audio'?'':null}, 'Preview Audio'),
      el('button', {onclick:async()=>await command({action:'stopBgm'}, false)}, 'Stop')));
    const duration = loop.background && loop.background.frames
      ? ' / ' + (loop.background.frames / (loop.background.sampleRate || 1)).toFixed(2) + ' sec' : '';
    bgm.append(el('p', {class:'hint'}, beatState.format === 'none' ? 'No Beat'
      : (beatState.format === 'audio' ? 'Audio: ' : 'Pattern: ') + (beatState.name || 'Untitled') + duration));
    bgm.append(rangeRow('Beat volume', 'beatPercent', beatState.volume, 100, v => command({action:'setLoop',beatVolume:v})));
    const settings = el('div', { class:'panel' }, el('h2', {}, 'Loop settings'));
    const length = el('input', { type:'number', min:250, max:8000, value:loop.lengthMs });
    const fixed = el('input', { type:'checkbox' }); fixed.checked = !!loop.lengthFixed;
    const quant = el('input', { type:'checkbox' }); quant.checked = !!loop.quantize;
    const grid = el('select', {}, ...[0,1,2,3,4].map(i => el('option',{value:i,selected:i===loop.noteGridIndex?'':null}, ['OFF','4','8','16','32'][i])));
    settings.append(el('div',{class:'row'},el('label',{},'Length ms'),length));
    settings.append(el('div',{class:'row'},el('label',{},'Fixed'),fixed));
    settings.append(el('div',{class:'row'},el('label',{},'Quantize'),quant));
    settings.append(el('div',{class:'row'},el('label',{},'Grid'),grid));
    settings.append(el('div',{class:'actions'},el('button',{class:'primary',onclick:async()=>await command({action:'setLoop',lengthMs:Number(length.value),lengthFixed:fixed.checked,quantize:quant.checked,noteGridIndex:Number(grid.value),noteOffGridIndex:loop.noteOffGridIndex})},'Apply')));
    root.append(el('div',{class:'grid'},bgm,settings), el('div',{class:'panel'},el('h2',{},'Beat files'),folderPanel('loops'),filePanel('loops','.wav,.mp3,.mid,.midi')));
  }
  function renderKit() {
    const root = $('#kit-view'); root.innerHTML='';
    const kit = el('div',{class:'panel'},el('h2',{},'Kit files'));
    const select = el('select',{},optionList('kits', files.kits,''));
    kit.append(el('div',{class:'row'},el('label',{},'Kit'),select));
    const importInput = el('input',{type:'file',accept:'.ksp,application/json'});
    kit.append(el('div',{class:'actions'},el('button',{class:'primary',onclick:async()=>{if(select.value) await command({action:'loadKit',file:browserFilePath('kits',select.value)});}},'Load'),el('button',{onclick:async()=>{const name=prompt('Kit file name','my-kit.json');if(name) await command({action:'saveKit',file:browserFilePath('kits',name.endsWith('.json')?name:name+'.json')});}},'Save current'),el('button',{class:'primary',onclick:exportKitPackage},'Export Kit'),importInput,el('button',{onclick:async()=>{if(importInput.files[0]) await importKitPackage(importInput.files[0]);}},'Import Kit')));
    root.append(el('div',{class:'notice'},'Export Kit includes the current Sampler audio, Beat, edit values and loop performance in one portable .ksp file.'),kit,el('div',{class:'panel'},el('h2',{},'Kit files'),folderPanel('kits'),filePanel('kits','.json')));
  }
  function cleanJsonName(name, fallback='New_Project') {
    const clean = String(name || fallback).replace(/[\\/]/g,'_').trim();
    return (clean || fallback).replace(/\.json$/i,'') + '.json';
  }
  function renderProject() {
    const root = $('#project-view');
    if (!root) return;
    root.innerHTML='';
    const current = state.project && state.project.file ? state.project.file.split('/').pop().replace(/\.json$/i,'') : 'New Project';
    const panel = el('div',{class:'panel'},el('h2',{},'Project'));
    const select = el('select',{},optionList('projects',files.projects,''));
    panel.append(el('p',{class:'project-current'},'Current: '+current));
    panel.append(el('div',{class:'row'},el('label',{},'Saved project'),select));
    panel.append(el('div',{class:'actions'},
      el('button',{class:'primary',onclick:async()=>{if(select.value) await command({action:'loadProject',file:browserFilePath('projects',select.value)});}},'Load'),
      el('button',{class:'primary',onclick:async()=>{
        const entered=prompt('Project file name',current === 'New Project' ? 'New_Project' : current);
        if(entered===null)return;
        const proposed=cleanJsonName(entered);
        if(files.projects.some(file=>file.name===proposed)&&!confirm('Overwrite '+proposed+'?'))return;
        await command({action:'saveProject',file:browserFilePath('projects',proposed)});
      }},'Save As'),
      el('button',{class:'danger',onclick:async()=>{if(confirm('Start a new Project? Unsaved changes will be lost.'))await command({action:'newProject'});}},'New Project')));
    const manage=el('div',{class:'panel'},el('h2',{},'Project files'),folderPanel('projects',false),filePanel('projects','.json'));
    root.append(el('div',{class:'notice'},'A Project stores the complete performance: audio, Beat, Rec, synth settings, FX and Mixer.'),panel,manage);
  }
  function safeKitName(name) { return (name || 'kit').replace(/[^a-z0-9_-]+/gi,'-').replace(/^-+|-+$/g,'').slice(0,40) || 'kit'; }
  function base64FromBlob(blob) { return new Promise((resolve,reject) => { const r=new FileReader(); r.onload=()=>resolve(String(r.result).split(',')[1]); r.onerror=reject; r.readAsDataURL(blob); }); }
  function blobFromBase64(data) { const bin=atob(data); const bytes=new Uint8Array(bin.length); for(let i=0;i<bin.length;i++) bytes[i]=bin.charCodeAt(i); return new Blob([bytes],{type:'audio/wav'}); }
  async function currentAudio(path) { return request('/api/sampler/audio/'+path).then(r=>r.blob()); }
  async function exportKitPackage() {
    if (PREVIEW) { status('Package export requires a connected sampler', true); return; }
    const name = safeKitName(prompt('Package name','my-kit'));
    if (!name) return;
    status('Collecting Kit audio…');
    const pack = {format:'kantan-sampler-package',version:1,name,assets:[],kit:{pads:[],loop:{...state.loop,background:{...state.loop.background}}}};
    for (const pad of state.pads) {
      if (!pad.frames) continue;
      const item={pad:pad.pad,name:pad.name,start:pad.start,end:pad.end,volume:pad.volume,pitch:pad.pitch,reverse:pad.reverse,hold:pad.hold,loop:pad.loop,file:pad.file};
      if (!String(pad.file).startsWith('builtin:')) {
        const asset='samples/pad'+String(pad.pad+1).padStart(2,'0')+'.wav';
        pack.assets.push({path:asset,data:await base64FromBlob(await currentAudio('pad/'+pad.pad+'.wav'))}); item.file=asset;
      }
      pack.kit.pads.push(item);
    }
    const bgm=state.loop.background;
    if (bgm.frames && !String(bgm.file).startsWith('builtin:')) {
      const asset='background.wav'; pack.assets.push({path:asset,data:await base64FromBlob(await currentAudio('background.wav'))}); pack.kit.loop.background.file=asset;
    }
    const blob=new Blob([JSON.stringify(pack)],{type:'application/json'});
    const a=el('a',{href:URL.createObjectURL(blob),download:name+'.ksp'});a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000);status('Kit package exported');
  }
  async function importKitPackage(file) {
    try {
      status('Importing Kit…'); const pack=JSON.parse(await file.text());
      if (pack.format !== 'kantan-sampler-package' || !pack.kit || !Array.isArray(pack.assets)) throw new Error('Not a KANTAN Sampler package');
      const name=safeKitName(pack.name || file.name.replace(/\.ksp$/i,''));
      await ensureFolder('samples','', 'Kits'); await ensureFolder('samples','Kits',name);
      await ensureFolder('loops','', 'Kits'); await ensureFolder('loops','Kits',name);
      const assetPath={};
      for (const asset of pack.assets) {
        const target=(asset.path === 'background.wav') ? '/sampler/loops/Kits/'+name+'/background.wav' : '/sampler/samples/Kits/'+name+'/'+asset.path.split('/').pop();
        const kind=asset.path === 'background.wav' ? 'loops' : 'samples';
        const relative=target.slice(rootFolder(kind).length+1);
        await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(relative),{method:'PUT',body:blobFromBase64(asset.data)}); assetPath[asset.path]=target;
      }
      const background={...pack.kit.loop.background,file:assetPath[pack.kit.loop.background.file] || pack.kit.loop.background.file};
      const kit={version:10,kind:'project',sampler:{volume:100},samples:pack.kit.pads.map(p=>({internalPad:p.pad,name:p.name,file:assetPath[p.file] || p.file,start:p.start,end:p.end,volume:p.volume,pitch:p.pitch,reverse:p.reverse,hold:p.hold,loop:p.loop})),beat:{format:background.file?'audio':'none',name:background.name || '',volume:100,repeats:background.repeats || 1},loop:{...pack.kit.loop,background}};
      const kitName=name+'.json'; const relative=relativeFolder('kits');
      await request('/api/sampler/files/kits/'+encodeURIComponent(relative ? relative+'/'+kitName : kitName),{method:'PUT',body:new Blob([JSON.stringify(kit)],{type:'application/json'})});
      await refresh(); status('Kit imported');
    } catch (err) { status('Import error: '+err.message,true); }
  }
  async function ensureFolder(kind, path, name) {
    try { await request('/api/sampler/folders/'+kind+'?path='+encodeURIComponent(path)+'&name='+encodeURIComponent(name),{method:'POST'}); }
    catch (err) { if (!String(err.message).includes('folder create failed')) throw err; }
  }
  function folderPanel(kind, selectable = true) {
    const current = activeFolder(kind);
    const selected = relativeFolder(kind);
    const up = current.includes('/') ? current.slice(0, current.lastIndexOf('/')) : '';
    const choose = el('select', {}, [el('option',{value:''}, current ? 'Open folder…' : 'Open folder…'), ...folders[kind].map(name => el('option',{value:name},name))]);
    choose.addEventListener('change', async () => { if (!choose.value) return; browseFolders[kind] = current ? current + '/' + choose.value : choose.value; await refresh(); });
    const use = selectable ? el('button',{class:'primary',onclick:async()=>{await command({action:'setFolder',kind,path:rootFolder(kind)+(current ? '/'+current : '')});}}, current === selected ? 'Selected' : 'Use this folder') : null;
    const back = el('button',{onclick:async()=>{browseFolders[kind]=up;await refresh();}},'Up'); back.disabled = !current;
    const create = el('button',{onclick:async()=>{const name=prompt('Folder name');if(name) await createFolder(kind,current,name);}},'New folder');
    return el('div',{class:'folder-panel'},el('div',{class:'folder-path'},'SD / '+kind+(current ? ' / '+current : '')),el('div',{class:'actions'},back,choose,use,create));
  }
  function filePanel(kind, accept) {
    const list = el('ul',{class:'file-list'});
    for (const file of builtinFiles(kind)) {
      const preview = el('button',{title:'Preview on sampler',onclick:async()=>await command({action:'previewWav',file:file.file,maxMs:1000},false)},'Play');
      list.append(el('li',{},el('span',{class:'name'},fileDisplayLabel(kind,file)),preview));
    }
    for (const file of files[kind]) {
      const preview = accept.includes('.wav')
        ? el('button',{title:'Preview on sampler',onclick:async()=>await command({action:'previewWav',file:state.folders[kind]+'/'+file.name,maxMs:1000},false)},'Play')
        : null;
      const download = el('button',{onclick:()=>downloadFile(kind,file.name)},'↓');
      const rename = el('button',{onclick:async()=>{
        let next=prompt('New file name',file.name);
        if(kind==='projects'&&next!==null)next=cleanJsonName(next);
        if(!next||next===file.name)return;
        try { await renameFile(kind,file.name,next); status('Renamed to '+next); }
        catch(err) { status('Rename failed: '+err.message,true); }
      }},'Rename');
      const remove = el('button',{class:'danger',onclick:async()=>{if(confirm('Delete '+file.name+'?')) await deleteFile(kind,file.name);}},'×');
      list.append(el('li',{},el('span',{class:'name'},fileDisplayLabel(kind,file)),el('small',{},Math.ceil(file.size/1024)+' KB'),preview,download,rename,remove));
    }
    const input = el('input',{type:'file',accept});
    const progressLabel = el('span',{class:'upload-progress-label'},'Preparing upload…');
    const progressValue = el('span',{class:'upload-progress-value'},'0%');
    const progressFill = el('span',{class:'upload-progress-fill'});
    const progress = el('div',{class:'upload-progress',hidden:''},
      el('div',{class:'upload-progress-head'},
        el('span',{class:'upload-spinner','aria-hidden':'true'}),progressLabel,progressValue),
      el('div',{class:'upload-progress-track'},progressFill));
    const showProgress = (percent, saving=false) => {
      progress.hidden=false;
      progress.classList.toggle('saving',saving);
      progressLabel.textContent=saving?'Saving to sampler…':'Uploading '+(input.files[0]?.name||'file')+'…';
      progressValue.textContent=saving?'':Math.max(0,Math.min(100,Math.round(percent)))+'%';
      progressFill.style.width=saving?'100%':Math.max(2,Math.min(100,percent))+'%';
    };
    const upload = el('button',{class:'primary',onclick:async()=>{
      const file=input.files[0]; if(!file)return;
      upload.disabled=true; input.disabled=true; showProgress(0);
      try { await uploadFile(kind,file,(percent,saving)=>showProgress(percent,saving)); input.value=''; status('Uploaded '+file.name); }
      catch(err) { status('Upload failed: '+err.message,true); }
      finally { upload.disabled=false; input.disabled=false; progress.hidden=true; progress.classList.remove('saving'); }
    }},'Upload');
    return el('div',{},el('div',{class:'row upload-row'},input,upload),progress,list);
  }
  async function renameFile(kind, name, next) {
    const relative = activeFolder(kind);
    const oldPath = rootFolder(kind)+'/'+(relative ? relative+'/' : '')+name;
    const newPath = rootFolder(kind)+'/'+(relative ? relative+'/' : '')+next;
    if (PREVIEW) {
      const file = previewFiles[kind].find(entry => entry.name === name);
      if (file) file.name = next;
      if(kind==='projects'&&previewState.project.file===oldPath)previewState.project.file=newPath;
      await refresh();
      return;
    }
    const path = relative ? relative + '/' + name : name;
    await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(path)+'?to='+encodeURIComponent(relative ? relative + '/' + next : next),{method:'POST'});
    if(kind==='projects')await command({action:'projectRenamed',old:oldPath,file:newPath});
    else await refresh();
  }
  async function deleteFile(kind, name) {
    if (PREVIEW) {
      previewFiles[kind] = previewFiles[kind].filter(file => file.name !== name);
      await refresh();
      return;
    }
    const path = activeFolder(kind); await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(path ? path + '/' + name : name),{method:'DELETE'});
    await refresh();
  }
  function uploadRequest(path,file,onProgress) {
    return new Promise((resolve,reject)=>{
      const xhr=new XMLHttpRequest();
      xhr.open('PUT',API+path);
      xhr.timeout=120000;
      xhr.upload.onprogress=e=>{
        if(e.lengthComputable)onProgress(e.loaded*100/e.total,false);
        if(e.lengthComputable&&e.loaded>=e.total)onProgress(100,true);
      };
      xhr.onload=()=>{
        if(xhr.status>=200&&xhr.status<300){resolve();return;}
        let message=xhr.statusText||'upload failed';
        try{message=JSON.parse(xhr.responseText).error||message;}catch(_){}
        reject(new Error(message));
      };
      xhr.onerror=()=>reject(new Error('connection lost'));
      xhr.ontimeout=()=>reject(new Error('upload timed out'));
      xhr.onabort=()=>reject(new Error('upload cancelled'));
      xhr.send(file);
    });
  }
  async function uploadFile(kind, file, onProgress=()=>{}) {
    if (PREVIEW) {
      onProgress(35,false); await sleep(180); onProgress(78,false); await sleep(180); onProgress(100,true); await sleep(220);
      const existing = previewFiles[kind].findIndex(entry => entry.name === file.name);
      const entry = {name:file.name, size:file.size};
      if (existing >= 0) previewFiles[kind][existing] = entry;
      else previewFiles[kind].push(entry);
      await refresh();
      return;
    }
    status('Uploading '+file.name+'…');
    const path = activeFolder(kind);
    await uploadRequest('/api/sampler/files/'+kind+'/'+encodeURIComponent(path ? path + '/' + file.name : file.name),file,onProgress);
    await refresh();
  }
  async function downloadFile(kind,name) {
    if (PREVIEW) {
      const a = el('a',{href:URL.createObjectURL(new Blob(['KANTAN Sampler preview file'], {type:'text/plain'})),download:name});
      a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000);
      return;
    }
    const path = activeFolder(kind); const blob=await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(path ? path + '/' + name : name)).then(r=>r.blob()); const a=el('a',{href:URL.createObjectURL(blob),download:name}); a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000);
  }
  async function createFolder(kind, current, name) {
    if (PREVIEW) {
      if (!previewFolders[kind].includes(name)) previewFolders[kind].push(name);
      await refresh();
      return;
    }
    const query = '?path=' + encodeURIComponent(current) + '&name=' + encodeURIComponent(name);
    await request('/api/sampler/folders/'+kind+query,{method:'POST'});
    await refresh();
  }
  function render() { if(!state)return; renderSamples();renderLoop();renderKit();renderProject(); }
  function setupTabs() { for(const tab of document.querySelectorAll('.tab')) tab.addEventListener('click',()=>{for(const t of document.querySelectorAll('.tab'))t.classList.toggle('active',t===tab);for(const v of document.querySelectorAll('.view'))v.classList.toggle('active',v.id===tab.dataset.view);}); }
  document.addEventListener('DOMContentLoaded',()=>{setupTabs();$('#refresh').addEventListener('click',refresh);refresh();});
})();
