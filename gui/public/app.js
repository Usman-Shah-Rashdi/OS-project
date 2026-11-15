// MiniFS Explorer – browser side.
// Gets the disk state from the server (/api/state), shows it like a file
// explorer, and sends changes back as MiniFS commands (/api/action).

'use strict';

// ---------------------------------------------------------------------------
// Icons (fixed SVG strings; user text is never put into innerHTML)
// ---------------------------------------------------------------------------

const stroke = (paths) =>
  `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">${paths}</svg>`;

const ICONS = {
  back: stroke('<path d="M15 18l-6-6 6-6"/>'),
  forward: stroke('<path d="M9 18l6-6-6-6"/>'),
  up: stroke('<path d="M12 19V5M6 11l6-6 6 6"/>'),
  refresh: stroke('<path d="M20 12a8 8 0 1 1-2.34-5.66M20 4v5h-5"/>'),
  newFolder: stroke('<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M12 10.5v5M9.5 13h5"/>'),
  newFile: stroke('<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5M12 11.5v5M9.5 14h5"/>'),
  cut: stroke('<circle cx="6" cy="18" r="3"/><circle cx="18" cy="18" r="3"/><path d="M8.1 15.9L19 4M15.9 15.9L5 4"/>'),
  copy: stroke('<rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h8"/>'),
  paste: stroke('<path d="M9 4H7a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V6a2 2 0 0 0-2-2h-2"/><rect x="9" y="2" width="6" height="4" rx="1"/>'),
  rename: stroke('<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M13.5 6.5l4 4"/>'),
  delete: stroke('<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>'),
  properties: stroke('<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5v.01"/>'),
  drive: stroke('<rect x="3" y="13" width="18" height="7" rx="2"/><path d="M5 13l2-8h10l2 8M17 16.5h.01"/>'),
  open: stroke('<path d="M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"/>'),
  newDisk: stroke('<rect x="3" y="13" width="18" height="7" rx="2"/><path d="M5 13l2-8h4M17 16.5h.01M17 3v6M14 6h6"/>'),
  pc: '<svg viewBox="0 0 24 24"><rect x="2.5" y="3.5" width="19" height="13" rx="1.5" fill="#4aa3df" stroke="#2f7fb8"/><rect x="4.5" y="5.5" width="15" height="9" fill="#9bd3f5"/><path d="M9 20.5h6M12 16.5v4" stroke="#6b7785" stroke-width="1.5" stroke-linecap="round"/></svg>',
  folder: '<svg viewBox="0 0 24 24"><path d="M2.5 6.5a2 2 0 0 1 2-2h4.6l2 2h8.4a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-15a2 2 0 0 1-2-2z" fill="#f2b73a"/><path d="M2.5 9.5h19v8a2 2 0 0 1-2 2h-15a2 2 0 0 1-2-2z" fill="#ffd25e"/></svg>',
  file: '<svg viewBox="0 0 24 24"><path d="M6 2.5h8.5l5 5V20a1.5 1.5 0 0 1-1.5 1.5H6A1.5 1.5 0 0 1 4.5 20V4A1.5 1.5 0 0 1 6 2.5z" fill="#fff" stroke="#9a9a9a"/><path d="M14.5 2.5v5h5" fill="#ececec" stroke="#9a9a9a" stroke-linejoin="round"/><path d="M8 12h8M8 15h8M8 18h5" stroke="#b5b5b5"/></svg>',
  disk: '<svg viewBox="0 0 24 24"><rect x="2.5" y="12" width="19" height="8" rx="2" fill="#d8dde3" stroke="#8b96a3"/><path d="M4.5 12l2.2-7.5h10.6l2.2 7.5" fill="#eef1f4" stroke="#8b96a3" stroke-linejoin="round"/><circle cx="17" cy="16" r="1.2" fill="#3fb34f"/></svg>',
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

const $ = (id) => document.getElementById(id);

function el(tag, props = {}, children = []) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (key === 'class') node.className = value;
    else if (key === 'text') node.textContent = value;
    else if (key === 'icon') node.insertAdjacentHTML('afterbegin', ICONS[value]);
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (value !== undefined && value !== false) node.setAttribute(key, value === true ? '' : value);
  }
  for (const child of [].concat(children)) {
    if (child !== null && child !== undefined) node.append(child);
  }
  return node;
}

const parentOf = (path) => {
  if (path === '/') return null;
  const i = path.lastIndexOf('/');
  return i <= 0 ? '/' : path.slice(0, i);
};
const nameOf = (path) => (path === '/' ? (view.disk?.name || 'Disk') : path.slice(path.lastIndexOf('/') + 1));
const DISKS = '#disks';  // location of the Disks page in the back/forward history
const joinPath = (dir, name) => (dir === '/' ? `/${name}` : `${dir}/${name}`);
const isDir = (item) => item.type === 'directory';
const modeOf = (item) => parseInt(item.mode, 8);

function typeName(item) {
  if (isDir(item)) return 'File folder';
  const name = nameOf(item.path);
  const dot = name.lastIndexOf('.');
  return dot > 0 ? `${name.slice(dot + 1).toUpperCase()} File` : 'File';
}

function formatDate(seconds) {
  return new Date(seconds * 1000).toLocaleString(undefined, {
    year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit',
  });
}

function formatSize(bytes) {
  if (bytes < 1024) return `${bytes} bytes`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

// Explorer shows file sizes in whole KB.
const listSize = (item) => (isDir(item) ? '' : `${Math.ceil(item.size / 1024).toLocaleString()} KB`);

function blockRanges(blocks) {
  const parts = [];
  for (let i = 0; i < blocks.length;) {
    let j = i;
    while (j + 1 < blocks.length && blocks[j + 1] === blocks[j] + 1) j++;
    parts.push(j > i ? `${blocks[i]}-${blocks[j]}` : `${blocks[i]}`);
    i = j + 1;
  }
  return parts.join(', ') || 'none';
}

// ---------------------------------------------------------------------------
// Server calls
// ---------------------------------------------------------------------------

async function action(name, args = [], extra = {}) {
  const response = await fetch('/api/action', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ action: name, args, ...extra }),
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || 'Something went wrong.');
  return data;
}

async function readFile(path) {
  const response = await fetch(`/api/read?path=${encodeURIComponent(path)}`);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error);
  return data.content;
}

async function diskRequest(name, body) {
  const response = await fetch(`/api/disks/${name}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || 'Something went wrong.');
  return data;
}

// Runs a change, reports errors, then reloads the view.
async function change(work, success) {
  try {
    await work();
    if (success) toast(success);
  } catch (error) {
    toast(error.message, true);
  }
  await refresh();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

const view = {
  data: null,          // last /api/state response
  raw: '',             // its text, to skip redraws when nothing changed
  disks: [],           // every disk image the server found
  disk: null,          // the open disk: { id, name, path }
  page: 'files',       // 'files' or 'disks'
  items: new Map(),    // path -> inode
  children: new Map(), // folder path -> child inodes
  cwd: '/',
  selected: null,
  back: [],
  forward: [],
  clipboard: null,     // { path, mode: 'copy' | 'cut' }
  expanded: new Set(['/']),
  sort: { key: 'name', desc: false },
};

function buildIndex(data) {
  view.items = new Map();
  view.children = new Map();
  for (const item of data.inodes) {
    if (!item.path) continue;
    view.items.set(item.path, item);
    if (isDir(item) && !view.children.has(item.path)) view.children.set(item.path, []);
    const parent = parentOf(item.path);
    if (parent !== null) {
      if (!view.children.has(parent)) view.children.set(parent, []);
      view.children.get(parent).push(item);
    }
  }
}

function sortedChildren(path) {
  const { key, desc } = view.sort;
  const items = [...(view.children.get(path) || [])];
  const compare = {
    name: (a, b) => nameOf(a.path).localeCompare(nameOf(b.path), undefined, { numeric: true }),
    modified: (a, b) => a.modified_at - b.modified_at,
    type: (a, b) => typeName(a).localeCompare(typeName(b)),
    size: (a, b) => a.size - b.size,
  }[key];
  items.sort((a, b) => {
    if (isDir(a) !== isDir(b)) return isDir(a) ? -1 : 1;  // folders first
    const result = compare(a, b) || nameOf(a.path).localeCompare(nameOf(b.path));
    return desc ? -result : result;
  });
  return items;
}

async function refresh({ quiet = false } = {}) {
  let text;
  let disksText;
  try {
    [text, disksText] = await Promise.all([
      fetch('/api/state').then((r) => r.text()),
      fetch('/api/disks').then((r) => r.text()),
    ]);
  } catch {
    if (!quiet) toast('Cannot reach the MiniFS server. Is "node gui/server.js" running?', true);
    return;
  }
  if (quiet && text + disksText === view.raw) return;
  view.raw = text + disksText;
  view.data = JSON.parse(text);
  view.disks = JSON.parse(disksText).disks;
  view.disk = view.data.disk;

  if (view.page === 'disks') {
    if (view.data.formatted) buildIndex(view.data);
    render();
    return;
  }
  if (!view.data.formatted) {
    showSetup();
    return;
  }
  buildIndex(view.data);
  if (!view.items.has(view.cwd) || !isDir(view.items.get(view.cwd))) view.cwd = '/';
  if (view.selected && !view.items.has(view.selected)) view.selected = null;
  if (view.clipboard && !view.items.has(view.clipboard.path)) view.clipboard = null;
  for (let p = view.cwd; p; p = parentOf(p)) view.expanded.add(p);
  render();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Where the user is: a folder path on the open disk, or the Disks page.
const here = () => (view.page === 'disks' ? DISKS : view.cwd);

function render() {
  const onDisks = view.page === 'disks';
  $('list').hidden = onDisks;
  $('setup').hidden = true;
  $('disks-page').hidden = !onDisks;
  renderAddress();
  renderSidebar();
  if (onDisks) {
    $('empty').hidden = true;
    renderDisksPage();
  } else {
    renderList();
  }
  renderStatus();
  renderButtons();
}

function renderAddress() {
  const address = $('address');
  address.replaceChildren(el('button', { text: 'Disks', onclick: () => navigate(DISKS) }));
  if (view.page === 'disks') return;
  const parts = [];
  for (let p = view.cwd; p; p = parentOf(p)) parts.unshift(p);
  for (const path of parts) {
    address.append(el('span', { class: 'chevron', text: '›' }));
    address.append(el('button', { text: nameOf(path), onclick: () => navigate(path) }));
  }
}

function treeRow({ depth, icon, label, title, current, toggle, onclick }) {
  const toggleBox = el('span', { class: 'toggle', text: toggle ? (toggle.open ? '▾' : '▸') : '' });
  if (toggle) {
    toggleBox.addEventListener('click', (event) => {
      event.stopPropagation();
      toggle.flip();
    });
  }
  return el('div', {
    class: `tree-item${current ? ' current' : ''}`,
    style: `padding-left:${4 + depth * 14}px`,
    title,
    onclick,
  }, [toggleBox, el('span', { class: 'icon', icon }), el('span', { class: 'label', text: label })]);
}

// Sidebar: "Disks", every disk under it, and the folders of the open disk.
function renderSidebar() {
  const tree = $('tree');
  const ready = view.data?.formatted;
  const flip = (path) => () => {
    if (view.expanded.has(path)) view.expanded.delete(path); else view.expanded.add(path);
    renderSidebar();
  };
  const addFolders = (path, depth) => {
    for (const folder of sortedChildren(path).filter(isDir)) {
      const hasSubfolders = sortedChildren(folder.path).some(isDir);
      tree.append(treeRow({
        depth,
        icon: 'folder',
        label: nameOf(folder.path),
        title: folder.path,
        current: view.page === 'files' && view.cwd === folder.path,
        toggle: hasSubfolders ? { open: view.expanded.has(folder.path), flip: flip(folder.path) } : null,
        onclick: () => navigate(folder.path),
      }));
      if (hasSubfolders && view.expanded.has(folder.path)) addFolders(folder.path, depth + 1);
    }
  };

  tree.replaceChildren(treeRow({
    depth: 0,
    icon: 'pc',
    label: 'Disks',
    title: 'All virtual disks',
    current: view.page === 'disks',
    onclick: () => navigate(DISKS),
  }));
  for (const disk of view.disks) {
    const isOpen = disk.id === view.disk?.id;
    const hasFolders = isOpen && ready && sortedChildren('/').some(isDir);
    tree.append(treeRow({
      depth: 1,
      icon: 'disk',
      label: disk.name,
      title: disk.path,
      current: isOpen && view.page === 'files' && view.cwd === '/',
      toggle: hasFolders ? { open: view.expanded.has('/'), flip: flip('/') } : null,
      onclick: () => (isOpen ? navigate('/') : openDisk(disk)),
    }));
    if (hasFolders && view.expanded.has('/')) addFolders('/', 2);
  }

  if (!ready) {
    $('usage').replaceChildren();
    return;
  }
  const { stats, superblock } = view.data;
  const used = stats.used_blocks * superblock.block_size;
  const total = stats.data_blocks * superblock.block_size;
  $('usage').replaceChildren(
    el('strong', { text: `${view.disk.name} (${superblock.allocation_mode})` }),
    el('div', { class: 'bar' }, el('span', { style: `width:${Math.max(stats.usage_percent, 0.5)}%` })),
    el('div', { text: `${formatSize(total - used)} free of ${formatSize(total)}` }),
  );
}

function renderList() {
  const rows = $('rows');
  rows.replaceChildren();
  const items = sortedChildren(view.cwd);
  for (const item of items) {
    const row = el('tr', {
      class: [item.path === view.selected ? 'selected' : '',
              view.clipboard?.mode === 'cut' && view.clipboard.path === item.path ? 'cut' : ''].join(' '),
      title: item.path,
      'data-path': item.path,
      onclick: () => select(item.path),
      ondblclick: () => open(item),
      oncontextmenu: (event) => {
        event.preventDefault();
        select(item.path);
        showItemMenu(event, item);
      },
    }, [
      el('td', {}, el('div', { class: 'name-cell', icon: isDir(item) ? 'folder' : 'file' },
                      el('span', { text: nameOf(item.path) }))),
      el('td', { text: formatDate(item.modified_at) }),
      el('td', { text: typeName(item) }),
      el('td', { class: 'num', text: listSize(item) }),
    ]);
    rows.append(row);
  }
  $('empty').hidden = items.length > 0;
  document.querySelectorAll('.list th').forEach((th) => {
    th.classList.toggle('sorted', th.dataset.sort === view.sort.key);
    th.classList.toggle('desc', th.dataset.sort === view.sort.key && view.sort.desc);
  });
}

// The Disks page: one card per disk image, like "This PC" in Explorer.
function renderDisksPage() {
  const cards = view.disks.map((disk) => {
    const used = disk.dataBytes - disk.freeBytes;
    const percent = disk.dataBytes ? (100 * used) / disk.dataBytes : 0;
    const isOpen = disk.id === view.disk?.id;
    return el('div', { class: `disk-card${isOpen ? ' current' : ''}`, title: disk.path,
                       ondblclick: () => openDisk(disk) }, [
      el('div', { class: 'top', icon: 'disk' }, el('div', { style: 'min-width:0' }, [
        el('div', { class: 'title' }, [
          disk.name,
          isOpen ? el('span', { class: 'badge', text: 'Open' }) : null,
          disk.supported ? null : el('span', { class: 'badge old', text: 'Old format' }),
        ]),
        el('div', { class: 'meta', text: `${disk.mode} allocation · ${formatSize(disk.fileSize)} file` }),
      ])),
      el('div', { class: 'bar' }, el('span', { style: `width:${Math.max(percent, 0.5)}%` })),
      el('div', { class: 'meta', text: `${formatSize(disk.freeBytes)} free of ${formatSize(disk.dataBytes)}` }),
      el('div', { class: 'meta', text: `Created ${formatDate(disk.createdAt)} · ${disk.id}` }),
      el('div', { class: 'actions' }, [
        el('button', { class: 'btn', text: 'Open', onclick: () => openDisk(disk) }),
        el('button', { class: 'btn danger', text: 'Delete', onclick: () => deleteDisk(disk) }),
      ]),
    ]);
  });
  $('disks-page').replaceChildren(
    el('div', { class: 'disks-head' }, [
      el('h2', { text: `Virtual disks (${view.disks.length})` }),
      el('button', { class: 'btn primary', text: 'New disk…', onclick: newDiskDialog }),
    ]),
    view.disks.length
      ? el('div', { class: 'disk-grid' }, cards)
      : el('p', { class: 'empty', text: 'No virtual disks yet. Click "New disk…" to make one.' }),
  );
}

function renderStatus() {
  if (view.page === 'disks') {
    $('status-left').textContent = `${view.disks.length} disk${view.disks.length === 1 ? '' : 's'}`;
  } else if (view.data?.formatted) {
    const count = (view.children.get(view.cwd) || []).length;
    let left = `${count} item${count === 1 ? '' : 's'}`;
    const item = view.items.get(view.selected);
    if (item) left += `    1 item selected${isDir(item) ? '' : `  ${formatSize(item.size)}`}`;
    $('status-left').textContent = left;
  } else {
    $('status-left').textContent = '';
  }
  $('status-right').textContent = view.disk?.path || '';
}

function renderButtons() {
  const onFiles = view.page === 'files' && Boolean(view.data?.formatted);
  const item = onFiles ? view.items.get(view.selected) : null;
  $('btn-back').disabled = view.back.length === 0;
  $('btn-forward').disabled = view.forward.length === 0;
  $('btn-up').disabled = view.page === 'disks';  // up from a disk's top folder goes to Disks
  $('btn-refresh').disabled = false;
  for (const id of ['btn-cut', 'btn-rename', 'btn-delete', 'btn-properties']) $(id).disabled = !item;
  $('btn-copy').disabled = !item || isDir(item);
  $('btn-paste').disabled = !onFiles || !view.clipboard;
  $('btn-new-folder').disabled = !onFiles;
  $('btn-new-file').disabled = !onFiles;
  $('btn-format').disabled = view.page === 'disks';
}

// Shown when the open disk does not exist yet or cannot be mounted.
function showSetup() {
  $('list').hidden = true;
  $('empty').hidden = true;
  $('disks-page').hidden = true;
  renderAddress();
  renderSidebar();
  renderStatus();
  renderButtons();
  const message = view.data.message || '';
  const old = message.includes('version');
  $('setup').hidden = false;
  $('setup').replaceChildren(
    el('div', { icon: 'drive' }),
    el('h2', { text: old ? `"${view.disk.name}" needs formatting` : `"${view.disk.name}" has not been created yet` }),
    el('p', { text: old
      ? 'This disk was made by an older version of MiniFS. Format it, or open another disk.'
      : 'Format it to create the disk, or make a new disk with a name of your choice.' }),
    el('div', { style: 'display:flex;gap:8px;justify-content:center;flex-wrap:wrap' }, [
      el('button', { class: 'btn primary', text: 'Format this disk…', onclick: formatDialog }),
      el('button', { class: 'btn', text: 'New disk…', onclick: newDiskDialog }),
      view.disks.length ? el('button', { class: 'btn', text: 'Show all disks', onclick: () => navigate(DISKS) }) : null,
    ]),
  );
}

// ---------------------------------------------------------------------------
// Navigation and selection
// ---------------------------------------------------------------------------

function navigate(target, record = true) {
  if (target === here()) return;
  if (record) {
    view.back.push(here());
    view.forward = [];
  }
  view.selected = null;
  if (target === DISKS) {
    view.page = 'disks';
  } else {
    view.page = 'files';
    view.cwd = view.items.has(target) ? target : '/';
    for (let p = view.cwd; p; p = parentOf(p)) view.expanded.add(p);
  }
  if (view.page === 'files' && !view.data?.formatted) showSetup();
  else render();
}

function goBack() {
  if (!view.back.length) return;
  view.forward.push(here());
  navigate(view.back.pop(), false);
}

function goForward() {
  if (!view.forward.length) return;
  view.back.push(here());
  navigate(view.forward.pop(), false);
}

function goUp() {
  if (view.page === 'disks') return;
  navigate(view.cwd === '/' ? DISKS : parentOf(view.cwd));
}

// Only moves the highlight: rebuilding the rows here would detach the row
// that is still handling the click.
function select(path) {
  view.selected = path;
  for (const row of $('rows').children) {
    row.classList.toggle('selected', row.dataset.path === path);
  }
  renderStatus();
  renderButtons();
}

function open(item) {
  if (isDir(item)) navigate(item.path);
  else openEditor(item);
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

let closeDialog = null;

function dialog({ title, body, buttons, wide = false, note = '' }) {
  if (closeDialog) closeDialog();
  const backdrop = $('backdrop');
  const close = () => {
    backdrop.hidden = true;
    backdrop.replaceChildren();
    closeDialog = null;
  };
  const buttonRow = el('div', { class: 'dialog-buttons' }, note ? el('span', { class: 'note', text: note }) : null);
  for (const button of buttons) {
    buttonRow.append(el('button', {
      class: `btn${button.primary ? ' primary' : ''}`,
      text: button.label,
      disabled: button.disabled,
      onclick: async () => {
        if (button.onClick && (await button.onClick()) === false) return;
        close();
      },
    }));
  }
  const box = el('div', { class: `dialog${wide ? ' wide' : ''}`, role: 'dialog', 'aria-modal': 'true' }, [
    el('h3', { text: title }),
    el('div', { class: 'dialog-body' }, body),
    buttonRow,
  ]);
  backdrop.replaceChildren(box);
  backdrop.hidden = false;
  backdrop.onclick = (event) => { if (event.target === backdrop) close(); };
  closeDialog = close;
  const focusTarget = box.querySelector('input, textarea, select') || box.querySelector('.btn.primary');
  if (focusTarget) setTimeout(() => focusTarget.focus(), 0);
  return close;
}

function ask(title, label, value) {
  return new Promise((resolve) => {
    const input = el('input', { type: 'text', value, maxlength: '59', spellcheck: 'false' });
    let answered = false;
    const finish = (result) => {
      if (!answered) {
        answered = true;
        resolve(result);
      }
    };
    const close = dialog({
      title,
      body: el('label', { class: 'field', text: label }, input),
      buttons: [
        { label: 'OK', primary: true, onClick: () => finish(input.value.trim() || null) },
        { label: 'Cancel', onClick: () => finish(null) },
      ],
    });
    input.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        finish(input.value.trim() || null);
        close();
      }
    });
    // Select the name without the extension, like Explorer.
    setTimeout(() => {
      const dot = value.lastIndexOf('.');
      input.setSelectionRange(0, dot > 0 ? dot : value.length);
    }, 0);
    const observer = new MutationObserver(() => {
      if ($('backdrop').hidden) {
        finish(null);
        observer.disconnect();
      }
    });
    observer.observe($('backdrop'), { attributes: true });
  });
}

function confirmBox(title, text, okLabel = 'Yes') {
  return new Promise((resolve) => {
    let answer = false;
    dialog({
      title,
      body: el('p', { text }),
      buttons: [
        { label: okLabel, primary: true, onClick: () => { answer = true; } },
        { label: 'No' },
      ],
    });
    const observer = new MutationObserver(() => {
      if ($('backdrop').hidden) {
        observer.disconnect();
        resolve(answer);
      }
    });
    observer.observe($('backdrop'), { attributes: true });
  });
}

// A name in the current folder that is not taken: "New folder (2)", ...
function freeName(base, folder = view.cwd) {
  const taken = new Set((view.children.get(folder) || []).map((item) => nameOf(item.path)));
  if (!taken.has(base)) return base;
  const dot = base.lastIndexOf('.');
  const stem = dot > 0 ? base.slice(0, dot) : base;
  const ext = dot > 0 ? base.slice(dot) : '';
  for (let n = 2; ; n++) {
    const name = `${stem} (${n})${ext}`;
    if (!taken.has(name)) return name;
  }
}

async function openEditor(item) {
  let content;
  try {
    content = await readFile(item.path);
  } catch (error) {
    toast(error.message, true);
    return;
  }
  const writable = (modeOf(item) & 0o200) !== 0;
  const textarea = el('textarea', { class: 'editor', spellcheck: 'false', readonly: !writable });
  textarea.value = content;
  const save = async () => {
    try {
      await action('write', [item.path, textarea.value]);
      toast(`Saved ${nameOf(item.path)}`);
      await refresh();
      return true;
    } catch (error) {
      toast(error.message, true);
      return false;
    }
  };
  textarea.addEventListener('keydown', async (event) => {
    if (event.ctrlKey && event.key.toLowerCase() === 's') {
      event.preventDefault();
      if (writable && (await save())) closeDialog?.();
    }
  });
  dialog({
    title: nameOf(item.path),
    body: textarea,
    wide: true,
    note: writable ? `${formatSize(item.size)} · Ctrl+S to save` : 'Read-only (no write permission)',
    buttons: [
      { label: 'Save', primary: true, disabled: !writable, onClick: save },
      { label: 'Close' },
    ],
  });
}

function showProperties(item) {
  const blockSize = view.data.superblock.block_size;
  const mode = modeOf(item);
  const readOnly = el('input', { type: 'checkbox', checked: (mode & 0o200) === 0 });
  const row = (label, value, gap = false) =>
    el('tr', { class: gap ? 'gap' : '' }, [el('td', { text: label }), el('td', { text: value })]);
  const diskBlocks = item.blocks.length + item.pointer_blocks.length;

  const table = el('table', { class: 'props' }, [
    row('Name', nameOf(item.path)),
    row('Type', typeName(item)),
    row('Location', parentOf(item.path) || '/'),
    row('Size', item.size < 1024 ? formatSize(item.size)
                                 : `${formatSize(item.size)} (${item.size.toLocaleString()} bytes)`),
    row('Size on disk', `${formatSize(diskBlocks * blockSize)} (${diskBlocks} block${diskBlocks === 1 ? '' : 's'})`),
    row('Created', formatDate(item.created_at), true),
    row('Modified', formatDate(item.modified_at)),
    row('Inode', String(item.inode), true),
    row('Permissions', `${isDir(item) ? 'd' : '-'}${item.permissions} (${item.mode.slice(1)})`),
    row('Links', String(item.links)),
    row('Data blocks', blockRanges(item.blocks)),
    item.pointer_blocks.length ? row('Indirect block', blockRanges(item.pointer_blocks)) : null,
    row('Fragments', item.blocks.length ? `${item.extents} (${item.extents === 1 ? 'contiguous' : 'fragmented'})` : '—'),
  ]);
  const attributes = el('label', { class: 'check' }, [readOnly, 'Read-only']);

  dialog({
    title: `${nameOf(item.path)} Properties`,
    body: [table, el('div', { class: 'props-attrs', style: 'margin-top:12px' }, attributes)],
    buttons: [
      {
        label: 'OK',
        primary: true,
        onClick: async () => {
          const wanted = readOnly.checked ? mode & ~0o222 : mode | 0o200;
          if (wanted !== mode) {
            await change(() => action('chmod', [wanted.toString(8).padStart(3, '0'), item.path]));
          }
        },
      },
      { label: 'Cancel' },
    ],
  });
}

function formatDialog() {
  const size = el('input', { type: 'number', min: '1', max: '1024', value: '100' });
  const mode = el('select', {}, [
    el('option', { value: 'indexed', text: 'Indexed (default)' }),
    el('option', { value: 'linked', text: 'Linked' }),
    el('option', { value: 'contiguous', text: 'Contiguous' }),
  ]);
  dialog({
    title: `Format "${view.disk?.name || 'disk'}"`,
    body: [
      view.data?.formatted ? el('p', { class: 'warning', text: 'This erases every file and folder on this disk.' }) : null,
      el('label', { class: 'field', text: 'Size (MB, 1–1024)' }, size),
      el('label', { class: 'field', text: 'Allocation method' }, mode),
    ],
    buttons: [
      {
        label: 'Format',
        primary: true,
        onClick: async () => {
          resetForDisk();
          await change(() => action('format', [], { mb: Number(size.value), alloc: mode.value }),
                       'Disk formatted');
        },
      },
      { label: 'Cancel' },
    ],
  });
}

// ---------------------------------------------------------------------------
// Disks
// ---------------------------------------------------------------------------

// Forget everything that belongs to the previously open disk.
function resetForDisk() {
  view.page = 'files';
  view.cwd = '/';
  view.selected = null;
  view.back = [];
  view.forward = [];
  view.clipboard = null;
  view.expanded = new Set(['/']);
}

async function openDisk(disk) {
  try {
    await diskRequest('open', { id: disk.id });
    resetForDisk();
  } catch (error) {
    toast(error.message, true);
  }
  await refresh();
}

function newDiskDialog() {
  const taken = new Set(view.disks.map((disk) => disk.name.toLowerCase()));
  let n = 1;
  while (taken.has(`disk ${n}`)) n++;
  const name = el('input', { type: 'text', value: `Disk ${n}`, maxlength: '40', spellcheck: 'false' });
  const size = el('input', { type: 'number', min: '1', max: '1024', value: '16' });
  const mode = el('select', {}, [
    el('option', { value: 'indexed', text: 'Indexed (default)' }),
    el('option', { value: 'linked', text: 'Linked' }),
    el('option', { value: 'contiguous', text: 'Contiguous' }),
  ]);
  dialog({
    title: 'New disk',
    body: [
      el('label', { class: 'field', text: 'Disk name' }, name),
      el('label', { class: 'field', text: 'Size (MB, 1–1024)' }, size),
      el('label', { class: 'field', text: 'Allocation method' }, mode),
    ],
    note: 'Saved in the disks folder',
    buttons: [
      {
        label: 'Create',
        primary: true,
        onClick: async () => {
          try {
            await diskRequest('create', { name: name.value.trim(), mb: Number(size.value), alloc: mode.value });
          } catch (error) {
            toast(error.message, true);
            return false;  // keep the dialog open to fix the input
          }
          resetForDisk();
          toast(`Created disk "${name.value.trim()}"`);
          await refresh();
          return true;
        },
      },
      { label: 'Cancel' },
    ],
  });
}

async function deleteDisk(disk) {
  const sure = await confirmBox('Delete disk',
    `Delete the disk "${disk.name}"? This permanently removes ${disk.id} and every file on it.`, 'Delete');
  if (!sure) return;
  const wasOpen = disk.id === view.disk?.id;
  try {
    await diskRequest('delete', { id: disk.id });
    toast(`Deleted disk "${disk.name}"`);
    if (wasOpen) resetForDisk();
    view.page = 'disks';
  } catch (error) {
    toast(error.message, true);
  }
  await refresh();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

async function newFolder() {
  const name = await ask('New folder', 'Folder name', freeName('New folder'));
  if (!name) return;
  await change(() => action('createdir', [joinPath(view.cwd, name)]));
  if (view.items.has(joinPath(view.cwd, name))) select(joinPath(view.cwd, name));
}

async function newFile() {
  const name = await ask('New file', 'File name', freeName('New Text Document.txt'));
  if (!name) return;
  await change(() => action('create', [joinPath(view.cwd, name)]));
  if (view.items.has(joinPath(view.cwd, name))) select(joinPath(view.cwd, name));
}

async function renameSelected() {
  const item = view.items.get(view.selected);
  if (!item) return;
  const name = await ask('Rename', 'New name', nameOf(item.path));
  if (!name || name === nameOf(item.path)) return;
  const target = joinPath(parentOf(item.path), name);
  await change(() => action('rename', [item.path, target]));
  if (view.items.has(target)) select(target);
}

async function deleteSelected() {
  const item = view.items.get(view.selected);
  if (!item) return;
  const text = isDir(item)
    ? `Delete the folder "${nameOf(item.path)}"? (Only empty folders can be deleted.)`
    : `Delete "${nameOf(item.path)}"? This cannot be undone.`;
  if (!(await confirmBox(isDir(item) ? 'Delete folder' : 'Delete file', text))) return;
  await change(() => action(isDir(item) ? 'deletedir' : 'delete', [item.path]));
}

function toClipboard(mode) {
  const item = view.items.get(view.selected);
  if (!item) return;
  if (mode === 'copy' && isDir(item)) {
    toast('Only files can be copied. Use Cut to move a folder.', true);
    return;
  }
  view.clipboard = { path: item.path, mode };
  toast(`${mode === 'cut' ? 'Cut' : 'Copied'} "${nameOf(item.path)}". Open a folder and choose Paste.`);
  renderList();
  renderButtons();
}

async function paste() {
  const clip = view.clipboard;
  if (!clip) return;
  const name = nameOf(clip.path);
  if (clip.mode === 'copy') {
    const taken = (view.children.get(view.cwd) || []).some((item) => nameOf(item.path) === name);
    let target = name;
    if (taken) {
      const dot = name.lastIndexOf('.');
      target = freeName(dot > 0 ? `${name.slice(0, dot)} - Copy${name.slice(dot)}` : `${name} - Copy`);
    }
    await change(() => action('copy', [clip.path, joinPath(view.cwd, target)]));
  } else {
    if (parentOf(clip.path) === view.cwd) {
      view.clipboard = null;
      renderList();
      renderButtons();
      return;
    }
    view.clipboard = null;
    await change(() => action('move', [clip.path, view.cwd]));
  }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

function showMenu(event, entries) {
  const menu = $('menu');
  menu.replaceChildren();
  for (const entry of entries) {
    if (entry === '-') {
      menu.append(el('hr'));
      continue;
    }
    menu.append(el('button', {
      icon: entry.icon,
      disabled: entry.disabled,
      onclick: () => {
        hideMenu();
        entry.run();
      },
    }, entry.label));
  }
  menu.hidden = false;
  const { innerWidth, innerHeight } = window;
  const rect = menu.getBoundingClientRect();
  menu.style.left = `${Math.min(event.clientX, innerWidth - rect.width - 8)}px`;
  menu.style.top = `${Math.min(event.clientY, innerHeight - rect.height - 8)}px`;
}

function hideMenu() {
  $('menu').hidden = true;
}

function showItemMenu(event, item) {
  showMenu(event, [
    { label: 'Open', icon: 'open', run: () => open(item) },
    '-',
    { label: 'Cut', icon: 'cut', run: () => toClipboard('cut') },
    { label: 'Copy', icon: 'copy', disabled: isDir(item), run: () => toClipboard('copy') },
    '-',
    { label: 'Rename', icon: 'rename', run: renameSelected },
    { label: 'Delete', icon: 'delete', run: deleteSelected },
    '-',
    { label: 'Properties', icon: 'properties', run: () => showProperties(item) },
  ]);
}

function showFolderMenu(event) {
  showMenu(event, [
    { label: 'New folder', icon: 'newFolder', run: newFolder },
    { label: 'New file', icon: 'newFile', run: newFile },
    '-',
    { label: 'Paste', icon: 'paste', disabled: !view.clipboard, run: paste },
    { label: 'Refresh', icon: 'refresh', run: () => refresh() },
  ]);
}

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------

let toastTimer = null;
function toast(text, isError = false) {
  const box = $('toast');
  box.textContent = text;
  box.className = `toast${isError ? ' error' : ''}`;
  box.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { box.hidden = true; }, isError ? 5000 : 2500);
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

document.querySelectorAll('[data-icon]').forEach((button) => {
  button.insertAdjacentHTML('afterbegin', ICONS[button.dataset.icon]);
});

$('btn-back').onclick = goBack;
$('btn-forward').onclick = goForward;
$('btn-up').onclick = goUp;
$('btn-refresh').onclick = () => refresh();
$('btn-new-folder').onclick = newFolder;
$('btn-new-file').onclick = newFile;
$('btn-cut').onclick = () => toClipboard('cut');
$('btn-copy').onclick = () => toClipboard('copy');
$('btn-paste').onclick = paste;
$('btn-rename').onclick = renameSelected;
$('btn-delete').onclick = deleteSelected;
$('btn-properties').onclick = () => {
  const item = view.items.get(view.selected);
  if (item) showProperties(item);
};
$('btn-format').onclick = formatDialog;

document.querySelectorAll('.list th').forEach((th) => {
  th.onclick = () => {
    const key = th.dataset.sort;
    view.sort = { key, desc: view.sort.key === key ? !view.sort.desc : false };
    renderList();
  };
});

// Clicking empty space clears the selection; right-click shows folder commands.
const onFilePage = () => view.page === 'files' && Boolean(view.data?.formatted);
$('content').addEventListener('click', (event) => {
  if (!event.target.closest('tbody tr') && onFilePage()) select(null);
});
$('content').addEventListener('contextmenu', (event) => {
  if (event.target.closest('tbody tr') || !onFilePage()) return;
  event.preventDefault();
  select(null);
  showFolderMenu(event);
});

document.addEventListener('click', (event) => {
  if (!event.target.closest('#menu')) hideMenu();
});
window.addEventListener('blur', hideMenu);

document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') {
    hideMenu();
    closeDialog?.();
    return;
  }
  if (closeDialog || event.target.closest('input, textarea, select')) return;
  const key = event.key;
  if (key === 'F5') {
    event.preventDefault();
    refresh();
    return;
  }
  if (key === 'ArrowLeft' && event.altKey) { goBack(); return; }
  if (key === 'ArrowRight' && event.altKey) { goForward(); return; }
  if (!onFilePage()) return;

  const item = view.items.get(view.selected);
  if (key === 'Delete' && item) deleteSelected();
  else if (key === 'F2' && item) renameSelected();
  else if (key === 'Enter' && event.altKey && item) showProperties(item);
  else if (key === 'Enter' && item) open(item);
  else if (key === 'Backspace') { event.preventDefault(); goUp(); }
  else if (event.ctrlKey && key.toLowerCase() === 'c') toClipboard('copy');
  else if (event.ctrlKey && key.toLowerCase() === 'x') toClipboard('cut');
  else if (event.ctrlKey && key.toLowerCase() === 'v') paste();
  else if (key === 'ArrowDown' || key === 'ArrowUp') {
    event.preventDefault();
    const items = sortedChildren(view.cwd);
    if (!items.length) return;
    const index = items.findIndex((entry) => entry.path === view.selected);
    const next = key === 'ArrowDown' ? Math.min(index + 1, items.length - 1) : Math.max(index - 1, 0);
    select(items[index === -1 ? 0 : next].path);
    $('rows').children[index === -1 ? 0 : next]?.scrollIntoView({ block: 'nearest' });
  }
});

// Pick up changes made from the MiniFS terminal while the page is open.
setInterval(() => {
  if (!document.hidden && !closeDialog) refresh({ quiet: true });
}, 3000);

refresh();
