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
    node.append(...children.flat().map(v => typeof v === 'string' ? document.createTextNode(v) : v));
    return node;
  };
  let state = null;
  let selectedPad = 0;
  let files = { samples: [], loops: [], kits: [] };
  let folders = { samples: [], loops: [], kits: [] };
  let browseFolders = { samples: '', loops: '', kits: '' };
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
      loop:{ lengthMs:4000, lengthFixed:true, quantize:true, noteGridIndex:4, noteOffGridIndex:4,
        background:{ file:'/sampler/loops/BGM_FA.wav', name:'BGM_FA', frames:192000, sampleRate:48000, volume:208 },
        events:[
          {pad:0,pos:0,type:'on',layer:0}, {pad:3,pos:500,type:'on',layer:0},
          {pad:1,pos:1000,type:'on',layer:0}, {pad:3,pos:1500,type:'on',layer:0},
          {pad:0,pos:2000,type:'on',layer:0}, {pad:4,pos:2250,type:'on',layer:0},
          {pad:1,pos:3000,type:'on',layer:0}, {pad:3,pos:3500,type:'on',layer:0}
        ] },
      folders:{ samples:'/sampler/samples', loops:'/sampler/loops', kits:'/sampler/kits' }
    };
  }
  const previewState = createPreviewState();
  const previewFiles = {
    samples:[
      {name:'kick-808.wav',size:48120}, {name:'snare-crisp.wav',size:45288},
      {name:'piko.wav',size:62640}, {name:'chin.wav',size:73728}, {name:'synth-penta.wav',size:95040}
    ],
    loops:[{name:'BGM_FA.wav',size:768000}, {name:'night-drive.wav',size:704000}],
    kits:[{name:'Starter Beat.json',size:2148}, {name:'Pentatonic Jam.json',size:2331}]
  };
  const previewFolders = { samples:['Drums', 'Synth'], loops:['Practice'], kits:['Favorites'] };

  function rootFolder(kind) { return '/sampler/' + kind; }
  function relativeFolder(kind, full = state && state.folders && state.folders[kind]) {
    const root = rootFolder(kind);
    return full && full.startsWith(root + '/') ? full.slice(root.length + 1) : '';
  }
  async function listFiles(kind) {
    const path = relativeFolder(kind);
    return request('/api/sampler/files/' + kind + (path ? '?path=' + encodeURIComponent(path) : '')).then(r => r.json());
  }
  async function listFolders(kind, relative = browseFolders[kind]) {
    const path = relative ? '?path=' + encodeURIComponent(relative) : '';
    return request('/api/sampler/folders/' + kind + path).then(r => r.json());
  }

  async function request(path, options = {}) {
    const res = await fetch(API + path, options);
    if (!res.ok) { let msg = res.statusText; try { msg = (await res.json()).error || msg; } catch (_) {} throw new Error(msg); }
    return res;
  }
  function status(text, error = false) { const n = $('#status'); n.textContent = text; n.style.color = error ? 'var(--danger)' : ''; }
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
    await new Promise(resolve => setTimeout(resolve, 180));
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
      const [samples, loops, kits, sampleFolders, loopFolders, kitFolders] = await Promise.all([
        listFiles('samples'), listFiles('loops'), listFiles('kits'),
        listFolders('samples'), listFolders('loops'), listFolders('kits')
      ]);
      files = { samples:samples.files, loops:loops.files, kits:kits.files };
      folders = { samples:sampleFolders.folders, loops:loopFolders.folders, kits:kitFolders.folders };
      loopEventsDraft = null;
      if (!state.pads.some(p => p.pad === selectedPad)) selectedPad = 0;
      render(); status('Connected');
    } catch (err) { status('Connection error: ' + err.message, true); }
  }
  function previewFileName(path) { return (path || '').split('/').pop().replace(/\.wav$/i, ''); }
  function applyPreviewCommand(payload) {
    const pad = previewState.pads.find(p => p.pad === payload.pad);
    if (payload.action === 'setPad' && pad) Object.assign(pad, payload);
    if (payload.action === 'clearPad' && pad) Object.assign(pad, previewPad(pad.pad, ''));
    if (payload.action === 'assignSample' && pad) {
      Object.assign(pad, previewPad(pad.pad, previewFileName(payload.file)));
    }
    if (payload.action === 'loadBgm') {
      const name = previewFileName(payload.file);
      previewState.loop.background = { file:payload.file, name, frames:192000, sampleRate:48000, volume:208 };
    }
    if (payload.action === 'clearBgm') previewState.loop.background = { file:'', name:'', frames:0, sampleRate:48000, volume:208 };
    if (payload.action === 'setLoop') {
      const patch = {...payload}; delete patch.action;
      if (patch.backgroundVolume !== undefined) {
        previewState.loop.background.volume = patch.backgroundVolume;
        delete patch.backgroundVolume;
      }
      Object.assign(previewState.loop, patch);
    }
    if (payload.action === 'setEvents') previewState.loop.events = payload.events.map(event => ({...event}));
    if (payload.action === 'saveKit') {
      const name = payload.file.split('/').pop();
      if (!previewFiles.kits.some(file => file.name === name)) previewFiles.kits.push({name, size:2200});
    }
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
    const button = el('button', { class:'pad' + (pad.pad === selectedPad ? ' selected' : '') + (!pad.frames ? ' empty' : ''), onclick:() => { selectedPad = pad.pad; renderSamples(); } },
      el('strong', {}, 'P' + pad.label), el('small', {}, pad.name || 'Empty'), waveSvg(pad));
    return button;
  }
  function optionList(items, selected, empty = 'Select file') {
    return [el('option', { value:'' }, empty), ...items.map(f => el('option', { value:f.name, selected:f.name === selected ? '' : null }, f.name))];
  }
  function renderSamples() {
    const root = $('#sample-view'); root.innerHTML = '';
    // 本体と同じ内部Pad順: 上段 P9-P12 / 中段 P5-P8 / 下段 P1-P4。
    const displayPads = [...state.pads].sort((a, b) => a.pad - b.pad);
    const grid = el('div', { class:'panel' }, el('h2', {}, 'Pads'), el('div', { class:'pad-grid' }, displayPads.map(padCard)));
    const pad = state.pads.find(p => p.pad === selectedPad) || state.pads[0];
    const edit = el('div', { class:'panel' }, el('h2', {}, 'Pad ' + pad.label + '  ' + (pad.name || 'Empty')));
    const sampleSelect = el('select', {}, optionList(files.samples, ''));
    edit.append(el('div', { class:'row' }, el('label', {}, 'WAV file'), sampleSelect));
    edit.append(el('div', { class:'actions' },
      el('button', { class:'primary', onclick:async() => { if (sampleSelect.value) await command({action:'assignSample',pad:pad.pad,file:state.folders.samples + '/' + sampleSelect.value}); } }, 'Assign'),
      el('button', { class:'danger', onclick:async() => await command({action:'clearPad',pad:pad.pad}) }, 'Clear'),
      el('button', { onclick:async() => await command(sampleSelect.value
        ? {action:'previewWav',file:state.folders.samples + '/' + sampleSelect.value}
        : {action:'playPad',pad:pad.pad}, false) }, 'Play'),
      el('button', { onclick:async() => await command({action:'stopAudio'}, false) }, 'Stop')));
    if (pad.frames) {
      const apply = async patch => command({ action:'setPad', pad:pad.pad, ...patch });
      edit.append(waveSvg(pad));
      edit.append(rangeRow('Start', 'start', pad.start, Math.max(1, pad.frames - 1), v => apply({start:v})));
      edit.append(rangeRow('End', 'end', pad.end, pad.frames, v => apply({end:v})));
      edit.append(rangeRow('Volume', 'volume', pad.volume, 512, v => apply({volume:v})));
      edit.append(rangeRow('Pitch', 'pitch', pad.pitch, 512, v => apply({pitch:Math.max(128,v)})));
      for (const [key, label] of [['reverse','Reverse'],['hold','Hold'],['loop','Loop']]) {
        const check = el('input', { type:'checkbox' }); check.checked = !!pad[key];
        check.addEventListener('change', () => apply({[key]:check.checked}));
        edit.append(el('div', { class:'row' }, el('label', {}, label), check));
      }
    } else edit.append(el('p', { class:'hint' }, 'Upload or select a WAV file, then assign it to this pad.'));
    const library = el('div', { class:'panel' }, el('h2', {}, 'Sample files'), folderPanel('samples'), filePanel('samples', '.wav'));
    root.append(el('div', { class:'grid' }, grid, edit), library);
  }
  function renderLoop() {
    const root = $('#loop-view'); root.innerHTML = '';
    const loop = state.loop;
    const bgm = el('div', { class:'panel' }, el('h2', {}, 'Background loop'));
    const select = el('select', {}, optionList(files.loops, loop.background.file.split('/').pop()));
    bgm.append(el('div', { class:'row' }, el('label', {}, 'BGM WAV'), select));
    bgm.append(el('div', { class:'actions' }, el('button', {class:'primary', onclick:async() => { if (select.value) await command({action:'loadBgm',file:state.folders.loops + '/' + select.value}); }}, 'Load BGM'), el('button', {class:'danger',onclick:async()=>await command({action:'clearBgm'})}, 'Clear BGM'), el('button', {onclick:async()=>await command({action:'playBgm'})}, 'Play BGM'), el('button', {onclick:async()=>await command({action:'stopBgm'})}, 'Stop')));
    bgm.append(el('p', {class:'hint'}, loop.background.name ? loop.background.name + ' / ' + (loop.background.frames / (loop.background.sampleRate || 1)).toFixed(2) + ' sec' : 'No BGM'));
    bgm.append(rangeRow('BGM volume', 'bgmVolume', loop.background.volume, 256, v => command({action:'setLoop',backgroundVolume:v})));
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
    root.append(el('div',{class:'grid'},bgm,settings), el('div',{class:'panel'},el('h2',{},'Loop files'),folderPanel('loops'),filePanel('loops','.wav')));
  }
  function renderKit() {
    const root = $('#kit-view'); root.innerHTML='';
    const kit = el('div',{class:'panel'},el('h2',{},'Kit files'));
    const select = el('select',{},optionList(files.kits,''));
    kit.append(el('div',{class:'row'},el('label',{},'Kit'),select));
    const importInput = el('input',{type:'file',accept:'.ksp,application/json'});
    kit.append(el('div',{class:'actions'},el('button',{class:'primary',onclick:async()=>{if(select.value) await command({action:'loadKit',file:state.folders.kits+'/'+select.value});}},'Load'),el('button',{onclick:async()=>{const name=prompt('Kit file name','my-kit.json');if(name) await command({action:'saveKit',file:state.folders.kits+'/'+(name.endsWith('.json')?name:name+'.json')});}},'Save current'),el('button',{class:'primary',onclick:exportKitPackage},'Export Kit'),importInput,el('button',{onclick:async()=>{if(importInput.files[0]) await importKitPackage(importInput.files[0]);}},'Import Kit')));
    root.append(el('div',{class:'notice'},'Export Kit includes the current pad audio, BGM, edit values and loop pattern in one portable .ksp file.'),kit,el('div',{class:'panel'},el('h2',{},'Kit files'),folderPanel('kits'),filePanel('kits','.json')));
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
      const kit={version:1,samples:pack.kit.pads.map(p=>({internalPad:p.pad,name:p.name,file:assetPath[p.file] || p.file,start:p.start,end:p.end,volume:p.volume,pitch:p.pitch,reverse:p.reverse,hold:p.hold,loop:p.loop})),loop:{...pack.kit.loop,background:{...pack.kit.loop.background,file:assetPath[pack.kit.loop.background.file] || pack.kit.loop.background.file}}};
      const kitName=name+'.json'; const relative=relativeFolder('kits');
      await request('/api/sampler/files/kits/'+encodeURIComponent(relative ? relative+'/'+kitName : kitName),{method:'PUT',body:new Blob([JSON.stringify(kit)],{type:'application/json'})});
      await refresh(); status('Kit imported');
    } catch (err) { status('Import error: '+err.message,true); }
  }
  async function ensureFolder(kind, path, name) {
    try { await request('/api/sampler/folders/'+kind+'?path='+encodeURIComponent(path)+'&name='+encodeURIComponent(name),{method:'POST'}); }
    catch (err) { if (!String(err.message).includes('folder create failed')) throw err; }
  }
  function folderPanel(kind) {
    const current = browseFolders[kind] || relativeFolder(kind);
    const selected = relativeFolder(kind);
    const up = current.includes('/') ? current.slice(0, current.lastIndexOf('/')) : '';
    const choose = el('select', {}, [el('option',{value:''}, current ? 'Open folder…' : 'Open folder…'), ...folders[kind].map(name => el('option',{value:name},name))]);
    choose.addEventListener('change', async () => { if (!choose.value) return; browseFolders[kind] = current ? current + '/' + choose.value : choose.value; await refresh(); });
    const use = el('button',{class:'primary',onclick:async()=>{await command({action:'setFolder',kind,path:rootFolder(kind)+(current ? '/'+current : '')});}}, current === selected ? 'Selected' : 'Use this folder');
    const back = el('button',{onclick:async()=>{browseFolders[kind]=up;await refresh();}},'Up'); back.disabled = !current;
    const create = el('button',{onclick:async()=>{const name=prompt('Folder name');if(name) await createFolder(kind,current,name);}},'New folder');
    return el('div',{class:'folder-panel'},el('div',{class:'folder-path'},'SD / '+kind+(current ? ' / '+current : '')),el('div',{class:'actions'},back,choose,use,create));
  }
  function filePanel(kind, accept) {
    const list = el('ul',{class:'file-list'});
    for (const file of files[kind]) {
      const download = el('button',{onclick:()=>downloadFile(kind,file.name)},'↓');
      const rename = el('button',{onclick:async()=>{const next=prompt('New file name',file.name);if(next&&next!==file.name) await renameFile(kind,file.name,next);}},'Rename');
      const remove = el('button',{class:'danger',onclick:async()=>{if(confirm('Delete '+file.name+'?')) await deleteFile(kind,file.name);}},'×');
      list.append(el('li',{},el('span',{class:'name'},file.name),el('small',{},Math.ceil(file.size/1024)+' KB'),download,rename,remove));
    }
    const input = el('input',{type:'file',accept});
    const upload = el('button',{class:'primary',onclick:async()=>{const file=input.files[0];if(!file)return;await uploadFile(kind,file);input.value='';}},'Upload');
    return el('div',{},el('div',{class:'row'},input,upload),list);
  }
  async function renameFile(kind, name, next) {
    if (PREVIEW) {
      const file = previewFiles[kind].find(entry => entry.name === name);
      if (file) file.name = next;
      await refresh();
      return;
    }
    const relative = relativeFolder(kind);
    const path = relative ? relative + '/' + name : name;
    await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(path)+'?to='+encodeURIComponent(relative ? relative + '/' + next : next),{method:'POST'});
    await refresh();
  }
  async function deleteFile(kind, name) {
    if (PREVIEW) {
      previewFiles[kind] = previewFiles[kind].filter(file => file.name !== name);
      await refresh();
      return;
    }
    const path = relativeFolder(kind); await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(path ? path + '/' + name : name),{method:'DELETE'});
    await refresh();
  }
  async function uploadFile(kind, file) {
    if (PREVIEW) {
      const existing = previewFiles[kind].findIndex(entry => entry.name === file.name);
      const entry = {name:file.name, size:file.size};
      if (existing >= 0) previewFiles[kind][existing] = entry;
      else previewFiles[kind].push(entry);
      await refresh();
      return;
    }
    status('Uploading…');
    const path = relativeFolder(kind); await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(path ? path + '/' + file.name : file.name),{method:'PUT',body:file});
    await refresh();
  }
  async function downloadFile(kind,name) {
    if (PREVIEW) {
      const a = el('a',{href:URL.createObjectURL(new Blob(['KANTAN Sampler preview file'], {type:'text/plain'})),download:name});
      a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000);
      return;
    }
    const path = relativeFolder(kind); const blob=await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(path ? path + '/' + name : name)).then(r=>r.blob()); const a=el('a',{href:URL.createObjectURL(blob),download:name}); a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000);
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
  function render() { if(!state)return; renderSamples();renderLoop();renderKit(); }
  function setupTabs() { for(const tab of document.querySelectorAll('.tab')) tab.addEventListener('click',()=>{for(const t of document.querySelectorAll('.tab'))t.classList.toggle('active',t===tab);for(const v of document.querySelectorAll('.view'))v.classList.toggle('active',v.id===tab.dataset.view);}); }
  document.addEventListener('DOMContentLoaded',()=>{setupTabs();$('#refresh').addEventListener('click',refresh);refresh();});
})();
