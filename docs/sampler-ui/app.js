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
        ] }
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

  async function request(path, options = {}) {
    const res = await fetch(API + path, options);
    if (!res.ok) { let msg = res.statusText; try { msg = (await res.json()).error || msg; } catch (_) {} throw new Error(msg); }
    return res;
  }
  function status(text, error = false) { const n = $('#status'); n.textContent = text; n.style.color = error ? 'var(--danger)' : ''; }
  async function command(payload) {
    if (PREVIEW) {
      applyPreviewCommand(payload);
      loopEventsDraft = null;
      render();
      status('Preview mode');
      return;
    }
    status('Applying…');
    await request('/api/sampler/command', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload) });
    await new Promise(resolve => setTimeout(resolve, 180));
    await refresh();
  }
  async function refresh() {
    if (PREVIEW) {
      state = previewState;
      files = previewFiles;
      loopEventsDraft = null;
      if (!state.pads.some(p => p.pad === selectedPad)) selectedPad = 0;
      render(); status('Preview mode');
      return;
    }
    try {
      const [s, samples, loops, kits] = await Promise.all([
        request('/api/sampler/state').then(r => r.json()),
        request('/api/sampler/files/samples').then(r => r.json()),
        request('/api/sampler/files/loops').then(r => r.json()),
        request('/api/sampler/files/kits').then(r => r.json()),
      ]);
      state = s; files = { samples:samples.files, loops:loops.files, kits:kits.files }; loopEventsDraft = null;
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
  function rangeRow(label, key, value, max, onChange) {
    const input = el('input', { type:'range', min:0, max, value });
    const out = el('span', { class:'range-value' }, String(value));
    input.addEventListener('input', () => { out.textContent = input.value; });
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
    const grid = el('div', { class:'panel' }, el('h2', {}, 'Pads'), el('div', { class:'pad-grid' }, state.pads.map(padCard)));
    const pad = state.pads.find(p => p.pad === selectedPad) || state.pads[0];
    const edit = el('div', { class:'panel' }, el('h2', {}, 'Pad ' + pad.label + '  ' + (pad.name || 'Empty')));
    const sampleSelect = el('select', {}, optionList(files.samples, ''));
    edit.append(el('div', { class:'row' }, el('label', {}, 'WAV file'), sampleSelect));
    edit.append(el('div', { class:'actions' },
      el('button', { class:'primary', onclick:async() => { if (sampleSelect.value) await command({action:'assignSample',pad:pad.pad,file:'/sampler/samples/' + sampleSelect.value}); } }, 'Assign'),
      el('button', { class:'danger', onclick:async() => await command({action:'clearPad',pad:pad.pad}) }, 'Clear')));
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
    const library = el('div', { class:'panel' }, el('h2', {}, 'Sample files'), filePanel('samples', '.wav'));
    root.append(el('div', { class:'grid' }, grid, edit), library);
  }
  function renderLoop() {
    const root = $('#loop-view'); root.innerHTML = '';
    const loop = state.loop;
    const bgm = el('div', { class:'panel' }, el('h2', {}, 'Background loop'));
    const select = el('select', {}, optionList(files.loops, loop.background.file.split('/').pop()));
    bgm.append(el('div', { class:'row' }, el('label', {}, 'BGM WAV'), select));
    bgm.append(el('div', { class:'actions' }, el('button', {class:'primary', onclick:async() => { if (select.value) await command({action:'loadBgm',file:'/sampler/loops/' + select.value}); }}, 'Load BGM'), el('button', {class:'danger',onclick:async()=>await command({action:'clearBgm'})}, 'Clear BGM')));
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
    const events = renderEvents(loop);
    root.append(el('div',{class:'grid'},bgm,settings), el('div',{class:'panel'},el('h2',{},'Loop events'),events), el('div',{class:'panel'},el('h2',{},'Loop files'),filePanel('loops','.wav')));
  }
  function renderEvents(loop) {
    const table = el('table',{class:'event-list'},el('thead',{},el('tr',{},el('th',{},'Pad'),el('th',{},'Time ms'),el('th',{},'Type'),el('th',{},''))),el('tbody',{}));
    const body = $('tbody',table);
    const rows = (loopEventsDraft || loop.events).map(e => ({...e}));
    const renderRow = (event,index) => {
      const pad = el('input',{type:'number',min:1,max:12,value:event.pad + 1});
      const pos = el('input',{type:'number',min:0,max:loop.lengthMs - 1,value:event.pos});
      const type = el('select',{},el('option',{value:'on'},'On'),el('option',{value:'off'},'Off')); type.value=event.type;
      pad.addEventListener('input',()=>event.pad=Math.max(0,Math.min(11,Number(pad.value)-1)));
      pos.addEventListener('input',()=>event.pos=Math.max(0,Number(pos.value)));
      type.addEventListener('change',()=>event.type=type.value);
      body.append(el('tr',{},el('td',{},pad),el('td',{},pos),el('td',{},type),el('td',{},el('button',{class:'danger',onclick:()=>{rows.splice(index,1); loopEventsDraft=rows; renderLoop();}},'×'))));
    };
    rows.forEach(renderRow);
    return el('div',{},table,el('div',{class:'actions'},el('button',{onclick:()=>{rows.push({pad:0,pos:0,type:'on',layer:0}); loopEventsDraft=rows; renderLoop();}},'Add event'),el('button',{class:'primary',onclick:async()=>await command({action:'setEvents',events:rows})},'Save events')));
  }
  function renderKit() {
    const root = $('#kit-view'); root.innerHTML='';
    const kit = el('div',{class:'panel'},el('h2',{},'Kit files'));
    const select = el('select',{},optionList(files.kits,''));
    kit.append(el('div',{class:'row'},el('label',{},'Kit'),select));
    kit.append(el('div',{class:'actions'},el('button',{class:'primary',onclick:async()=>{if(select.value) await command({action:'loadKit',file:'/sampler/kits/'+select.value});}},'Load'),el('button',{onclick:async()=>{const name=prompt('Kit file name','my-kit.json');if(name) await command({action:'saveKit',file:'/sampler/kits/'+(name.endsWith('.json')?name:name+'.json')});}},'Save current')));
    root.append(el('div',{class:'notice'},'Kit files store pad assignments, pad edit values, BGM and loop events. Recorded audio without an SD WAV file is not included.'),kit,el('div',{class:'panel'},el('h2',{},'Kit files'),filePanel('kits','.json')));
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
    await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(name)+'?to='+encodeURIComponent(next),{method:'POST'});
    await refresh();
  }
  async function deleteFile(kind, name) {
    if (PREVIEW) {
      previewFiles[kind] = previewFiles[kind].filter(file => file.name !== name);
      await refresh();
      return;
    }
    await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(name),{method:'DELETE'});
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
    await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(file.name),{method:'PUT',body:file});
    await refresh();
  }
  async function downloadFile(kind,name) {
    if (PREVIEW) {
      const a = el('a',{href:URL.createObjectURL(new Blob(['KANTAN Sampler preview file'], {type:'text/plain'})),download:name});
      a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000);
      return;
    }
    const blob=await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(name)).then(r=>r.blob()); const a=el('a',{href:URL.createObjectURL(blob),download:name}); a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000);
  }
  function render() { if(!state)return; renderSamples();renderLoop();renderKit(); }
  function setupTabs() { for(const tab of document.querySelectorAll('.tab')) tab.addEventListener('click',()=>{for(const t of document.querySelectorAll('.tab'))t.classList.toggle('active',t===tab);for(const v of document.querySelectorAll('.view'))v.classList.toggle('active',v.id===tab.dataset.view);}); }
  document.addEventListener('DOMContentLoaded',()=>{setupTabs();$('#refresh').addEventListener('click',refresh);refresh();});
})();
