(() => {
  const API = (window.KANPLAY && window.KANPLAY.api) || location.origin;
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

  function el(tag, attrs, ...children) {
    const e = document.createElement(tag);
    if (attrs) {
      for (const k in attrs) {
        if (k === 'class') e.className = attrs[k];
        else if (k.startsWith('on')) e.addEventListener(k.slice(2), attrs[k]);
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

    const btnDownload = el('button', { onclick: () => doDownload(f.name) }, 'Download');
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
        li.appendChild(btnRename);
        li.appendChild(btnDelete);
      }
    }

    li.appendChild(chk);
    li.appendChild(nameSpan);
    li.appendChild(sizeSpan);
    li.appendChild(btnDownload);
    li.appendChild(btnRename);
    li.appendChild(btnDelete);
    return li;
  }

  async function doDownload(name) {
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

  render();
})();
