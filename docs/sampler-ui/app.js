(() => {
  const API = (window.KANPLAY && window.KANPLAY.api) || location.origin;
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

  async function request(path, options = {}) {
    const res = await fetch(API + path, options);
    if (!res.ok) { let msg = res.statusText; try { msg = (await res.json()).error || msg; } catch (_) {} throw new Error(msg); }
    return res;
  }
  function status(text, error = false) { const n = $('#status'); n.textContent = text; n.style.color = error ? 'var(--danger)' : ''; }
  async function command(payload) {
    status('Applying…');
    await request('/api/sampler/command', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload) });
    await new Promise(resolve => setTimeout(resolve, 180));
    await refresh();
  }
  async function refresh() {
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
      const remove = el('button',{class:'danger',onclick:async()=>{if(confirm('Delete '+file.name+'?')){await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(file.name),{method:'DELETE'});await refresh();}}},'×');
      list.append(el('li',{},el('span',{class:'name'},file.name),el('small',{},Math.ceil(file.size/1024)+' KB'),download,remove));
    }
    const input = el('input',{type:'file',accept});
    const upload = el('button',{class:'primary',onclick:async()=>{const file=input.files[0];if(!file)return;status('Uploading…');await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(file.name),{method:'PUT',body:file});input.value='';await refresh();}},'Upload');
    return el('div',{},el('div',{class:'row'},input,upload),list);
  }
  async function downloadFile(kind,name) { const blob=await request('/api/sampler/files/'+kind+'/'+encodeURIComponent(name)).then(r=>r.blob()); const a=el('a',{href:URL.createObjectURL(blob),download:name}); a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000); }
  function render() { if(!state)return; renderSamples();renderLoop();renderKit(); }
  function setupTabs() { for(const tab of document.querySelectorAll('.tab')) tab.addEventListener('click',()=>{for(const t of document.querySelectorAll('.tab'))t.classList.toggle('active',t===tab);for(const v of document.querySelectorAll('.view'))v.classList.toggle('active',v.id===tab.dataset.view);}); }
  document.addEventListener('DOMContentLoaded',()=>{setupTabs();$('#refresh').addEventListener('click',refresh);refresh();});
})();
