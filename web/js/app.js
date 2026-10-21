// TreeFern for the web: wires the document, the tree view and the page UI.
import { Doc, fromJson, toJson, makeNode, walk, depth, countDesc, contains } from './model.js';
import { TreeView, THEMES } from './view.js';
import { STANDARD, DISKS, DEFAULT_SPREAD, DEFAULT_SHRINK, DEFAULT_CURL } from './layout.js';
import { DEG } from './geom.js';

const SAMPLES = [
    { file: 'life.json', title: 'Tree of life' },
    { file: 'apes.json', title: 'Apes' },
    { file: 'reptiles.json', title: 'Reptiles' },
    { file: 'indo-european.json', title: 'Indo-European languages' },
];
const MAX_SEARCH = 200;
// A development build (see build-site.sh) shares the site's origin, so it keeps separate storage.
const CHANNEL = document.querySelector('meta[name="treefern-channel"]')?.content || '';
const SESSION_KEY = CHANNEL ? `treefern.${CHANNEL}.session` : 'treefern.session';
const PREFS_KEY = CHANNEL ? `treefern.${CHANNEL}.prefs` : 'treefern.prefs';
const $ = id => document.getElementById(id);
const narrow = matchMedia('(max-width: 760px)');
const coarse = matchMedia('(pointer: coarse)');

const store = {  // localStorage can be missing or full; the app works without it
    get(k) { try { return JSON.parse(localStorage.getItem(k)); } catch { return null; } },
    set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); return true; } catch { return false; } },
};

const doc = new Doc();
let fileHandle = null;   // File System Access handle: edits are written back to the file
let status = '';
let sessionTimer = 0, fileTimer = 0;

const view = new TreeView($('tree'), doc, {
    select: id => select(id),
    edit: id => { select(id); actEdit(); },
    menu: (id, x, y) => { if (id) select(id); showMenu(id ? cladeItems() : canvasItems(), { x, y }); },
    viewChanged: () => updateHud(),
    focus: () => { if (document.activeElement !== $('tree')) $('tree').focus({ preventScroll: true }); },
});
$('tree').tabIndex = 0;
window.treefern = { doc, view };  // for debugging from the console

// ---------------------------------------------------------------- state
const sel = () => doc.find(view.selId);
function setStatus(s, toast = false) {
    status = s;
    updateHud();
    if (toast) showToast(s);
}
function showToast(s) {
    const t = $('toast');
    t.textContent = s;
    t.hidden = false;
    clearTimeout(showToast.timer);
    showToast.timer = setTimeout(() => { t.hidden = true; }, 2600);
}

doc.onChange = kind => {
    if (kind === 'open') {
        view.selId = view.cutId = view.hoverId = '';
        view.cam.anchor = '';
        view.relayout(false);
        view.fitView();
    } else {
        view.relayout(true);
    }
    refresh();
    if (!$('results').hidden) runSearch();  // keep open result lists current
    saveSession();
    if (kind === 'edit' && fileHandle) {
        clearTimeout(fileTimer);
        fileTimer = setTimeout(() => writeHandle(fileHandle), 600);
    }
};

function saveSession() {
    clearTimeout(sessionTimer);
    sessionTimer = setTimeout(() => {
        if (doc.root && !store.set(SESSION_KEY, { name: doc.name, json: doc.current }))
            setStatus('Could not keep a copy in this browser; use Save or Download', true);
    }, 300);
}

// Drop stale ids, then sync the panel, buttons and HUD.
function refresh() {
    if (!doc.find(view.cutId)) view.cutId = '';
    if (!sel()) view.selId = '';
    renderPanel();
    $('undoBtn').disabled = !doc.undoStack.length;
    $('redoBtn').disabled = !doc.redoStack.length;
    $('docName').textContent = doc.name;
    document.title = (doc.name ? doc.name + ' - ' : '') + 'TreeFern' + (CHANNEL ? ` (${CHANNEL})` : '');
    $('hint').textContent = view.selId || !doc.root ? '' :
        'Click a node to select · wheel to zoom · drag to pan · double-click to edit · right-click for menu · ? for help';
    view.draw();
    updateHud();
}

function updateHud() {
    const n = doc.root ? view.lay.count() : 0;
    const L = view.lay;
    const cut = doc.find(view.cutId);
    const parts = [view.disks() ? 'Disks' : 'Standard', 'zoom ' + view.zoomText(), `${n} nodes`];
    if (!view.disks() && !narrow.matches)
        parts.push(`spread ${Math.round(L.spread / DEG)}°  shrink ${L.shrink.toFixed(2)}  curl ${L.curl.toFixed(2)}`);
    if (cut) parts.push(`cut: ${cut.name} (select a target, then Paste)`);
    if (status) parts.push(status);
    $('hud').textContent = parts.join('   ·   ');
}

// ---------------------------------------------------------------- panel
const ACTIONS = [
    { id: 'add', label: 'Add child', key: 'A', run: () => actAdd(), ok: n => !!n },
    { id: 'edit', label: 'Edit', key: 'E', run: () => actEdit(), ok: n => !!n },
    { id: 'collapse', label: n => n && n.collapsed ? 'Expand' : 'Collapse', key: 'C', run: () => actCollapse(), ok: n => !!n && n.kids.length > 0 },
    { id: 'focus', label: 'Focus', key: 'F', run: () => { const n = sel(); if (n) view.focusOn(n); }, ok: n => !!n },
    { id: 'cut', label: 'Cut', key: 'X', run: () => actCut(), ok: n => !!n && !!n.parent },
    { id: 'paste', label: 'Paste here', key: 'V', run: () => actPaste(), ok: n => { const c = doc.find(view.cutId); return !!n && !!c && !contains(c, n); } },
    { id: 'delete', label: 'Delete', key: 'Del', run: () => actDelete(), ok: n => !!n && !!n.parent, danger: true },
];
const labelOf = (a, n) => typeof a.label === 'function' ? a.label(n) : a.label;

function buildActions() {
    const box = $('actions');
    for (const a of ACTIONS) {
        const b = document.createElement('button');
        b.dataset.act = a.id;
        if (a.danger) b.className = 'danger';
        b.addEventListener('click', () => a.run());
        box.append(b);
    }
}

function renderPanel() {
    const n = sel();
    $('empty').hidden = !!n;
    $('details').hidden = !n;
    for (const a of ACTIONS) {
        const b = $('actions').querySelector(`[data-act="${a.id}"]`);
        b.innerHTML = '';
        b.append(labelOf(a, n));
        const k = document.createElement('kbd');
        k.textContent = a.key;
        b.append(k);
        b.disabled = !a.ok(n);
    }
    if (!n) return;
    $('title').textContent = n.name;
    $('meta').textContent = `Depth: ${depth(n)}\nParent: ${n.parent ? n.parent.name : '(root)'}\n` +
        `Direct subclades: ${n.kids.length}\nTotal descendants: ${countDesc(n)}`;
    const up = $('upBtn');
    up.hidden = !n.parent;
    up.textContent = n.parent ? '‹ up to ' + n.parent.name : '';
    const kids = $('kids');
    kids.innerHTML = '';
    for (const k of n.kids) {
        const li = document.createElement('li');
        li.textContent = '› ' + k.name;
        li.addEventListener('click', () => goTo(k));
        kids.append(li);
    }
    if (!n.kids.length) { const li = document.createElement('li'); li.textContent = '(none)'; li.style.color = 'var(--dim)'; li.style.cursor = 'default'; kids.append(li); }
    const info = $('info');
    info.textContent = n.info || 'No info yet. Use Edit to add some.';
    info.classList.toggle('none', !n.info);
}

$('upBtn').addEventListener('click', () => { const n = sel(); if (n && n.parent) goTo(n.parent); });
$('sheetHandle').addEventListener('click', () => $('panel').classList.toggle('expanded'));
$('title').addEventListener('click', () => { if (narrow.matches) $('panel').classList.toggle('expanded'); });

// Keep the tree centred in the part of the canvas the bottom sheet leaves free.
new ResizeObserver(() => {
    view.insetBottom = narrow.matches ? $('panel').getBoundingClientRect().height : 0;
}).observe($('panel'));

// ---------------------------------------------------------------- selection & navigation
function select(id) {
    view.selId = id || '';
    refresh();
}
function goTo(n) {
    view.selId = n.id;
    view.ensureVisible(n);
    refresh();
}
function jumpTo(id) {
    let n = doc.find(id);
    if (!n) return;
    let expanded = false;
    for (let a = n.parent; a; a = a.parent) if (a.collapsed) { a.collapsed = false; expanded = true; }
    view.selId = id;
    if (expanded) doc.commit();
    if ((n = doc.find(id))) view.focusOn(n);
    refresh();
}

function navigate(d) {
    const n = sel();
    if (!n) { if (doc.root) goTo(doc.root); return; }
    let target = null;
    if (d === 'mother') {
        if (!(target = n.parent)) setStatus('Already at the root clade');
    } else if (d === 'daughter') {
        if (!n.kids.length) setStatus(`'${n.name}' has no daughters`);
        else if (n.collapsed) setStatus(`'${n.name}' is collapsed. Press C to expand`);
        else target = view.lay.ordered(n).at(-1);
    } else if (!n.parent) {
        setStatus('The root clade has no sisters');
    } else {
        const sis = view.lay.ordered(n.parent);
        if (sis.length < 2) setStatus(`'${n.name}' has no sisters`);
        else {
            const i = sis.indexOf(n);
            target = sis[(i + (d === 'right' ? 1 : -1) + sis.length) % sis.length];
        }
    }
    if (target) goTo(target);
    else refresh();
}

// ---------------------------------------------------------------- editing
let dlgResolve = null;
function openEditDialog({ title, name = '', info = '', withInfo = true }) {
    const dlg = $('editDlg');
    $('editTitle').textContent = title;
    $('editName').value = name;
    $('editInfo').value = info;
    $('editInfoRow').hidden = !withInfo;
    dlg.showModal();
    $('editName').focus();
    $('editName').select();
    return new Promise(res => { dlgResolve = res; });
}
$('editForm').addEventListener('submit', e => {
    e.preventDefault();
    finishEdit(true);
});
$('editCancel').addEventListener('click', () => finishEdit(false));
function finishEdit(ok) {
    const name = $('editName').value.trim(), info = $('editInfo').value;
    if (ok && !name) { $('editName').focus(); return; }
    $('editDlg').close();
    const r = dlgResolve;
    dlgResolve = null;
    r?.(ok ? { name, info } : null);
}
$('editDlg').addEventListener('cancel', e => { e.preventDefault(); finishEdit(false); });
$('editDlg').addEventListener('keydown', e => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); finishEdit(true); }
});

// Dialogs give an external change no chance here, but a node may be gone
// after an undo in another tab; re-look it up by id like the desktop app.
async function actAdd() {
    let n = sel();
    if (!n) return;
    const id = n.id;
    const r = await openEditDialog({ title: `New clade under '${n.name}'`, withInfo: true });
    if (!r || !(n = doc.find(id))) return;
    const c = doc.addChild(n, r.name, r.info);
    view.selId = c.id;
    refresh();
}

async function actEdit() {
    let n = sel();
    if (!n) return;
    const id = n.id;
    const r = await openEditDialog({ title: 'Edit clade', name: n.name, info: n.info });
    if (!r || !(n = doc.find(id))) return;
    if (r.name === n.name && r.info === n.info) return;
    n.name = r.name;
    n.info = r.info;
    doc.commit();
}

function actCollapse() {
    const n = sel();
    if (!n || !n.kids.length) return;
    n.collapsed = !n.collapsed;
    doc.commit();
}

function actDelete() {
    const n = sel();
    if (!n) return;
    const parentId = n.parent ? n.parent.id : '';
    const name = n.name;
    const err = doc.removeSubtree(n);
    if (err) return setStatus(err, true);
    view.selId = parentId;
    setStatus(`Deleted '${name}'. Undo with Ctrl+Z or the undo button`);
    refresh();
}

function actCut() {
    const n = sel();
    if (!n) return;
    if (!n.parent) setStatus('Cannot cut the root clade', true);
    else {
        view.cutId = n.id;
        setStatus(`Cut '${n.name}' (+${countDesc(n)} descendants). Select a new parent and Paste`, coarse.matches);
    }
    refresh();
}

function actPaste() {
    const target = sel(), cut = doc.find(view.cutId);
    if (!target || !cut) return;
    const name = cut.name, tname = target.name;
    const err = doc.moveSubtree(cut, target);
    if (err) setStatus(err, true);
    else {
        view.selId = view.cutId;
        view.cutId = '';
        setStatus(`Moved '${name}' under '${tname}'`);
    }
    refresh();
}

function undo() { if (!doc.undo()) setStatus('Nothing to undo'); else setStatus('Undone'); }
function redo() { if (!doc.redo()) setStatus('Nothing to redo'); else setStatus('Redone'); }

// ---------------------------------------------------------------- files
function loadJsonText(text, name) {
    let root;
    try { root = fromJson(JSON.parse(text)); }
    catch (e) { setStatus(`Cannot open ${name}: ${e.message}`, true); return false; }
    doc.load(root, name);
    return true;
}

async function openSample(s) {
    try {
        const r = await fetch('samples/' + s.file);
        if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
        if (loadJsonText(await r.text(), s.file)) {
            fileHandle = null;
            setStatus(`Opened the ${s.title} sample`);
        }
    } catch (e) { setStatus(`Cannot load the sample: ${e.message}`, true); }
}

async function openFile() {
    if (window.showOpenFilePicker) {
        try {
            const [h] = await showOpenFilePicker({ types: [{ description: 'Cladogram JSON', accept: { 'application/json': ['.json'] } }] });
            const f = await h.getFile();
            if (loadJsonText(await f.text(), f.name)) {
                fileHandle = h;
                setStatus(`Opened ${f.name}. Edits are saved to the file`);
            }
        } catch (e) { if (e.name !== 'AbortError') setStatus('Cannot open the file: ' + e.message, true); }
        return;
    }
    $('fileInput').click();
}
$('fileInput').addEventListener('change', async e => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) await openPlainFile(f);
});
async function openPlainFile(f) {
    if (loadJsonText(await f.text(), f.name)) {
        fileHandle = null;
        setStatus(`Opened ${f.name}. Edits are kept in this browser; use Save to download`);
    }
}

const jsonText = () => JSON.stringify(toJson(doc.root), null, 4) + '\n';

async function writeHandle(h) {
    try {
        const w = await h.createWritable();
        await w.write(jsonText());
        await w.close();
        setStatus('Saved ' + new Date().toLocaleTimeString());
        return true;
    } catch (e) {
        setStatus('Cannot write the file: ' + e.message, true);
        return false;
    }
}

function download(blob, name) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

async function save() {
    if (!doc.root) return;
    if (fileHandle) return writeHandle(fileHandle);
    return saveAs();
}

async function saveAs() {
    if (!doc.root) return;
    const name = doc.name && doc.name.endsWith('.json') ? doc.name : (doc.name || 'cladogram') + '.json';
    if (window.showSaveFilePicker) {
        try {
            const h = await showSaveFilePicker({ suggestedName: name, types: [{ description: 'Cladogram JSON', accept: { 'application/json': ['.json'] } }] });
            if (await writeHandle(h)) {
                fileHandle = h;
                doc.name = h.name;
                saveSession();
                refresh();
            }
        } catch (e) { if (e.name !== 'AbortError') setStatus('Cannot save: ' + e.message, true); }
        return;
    }
    download(new Blob([jsonText()], { type: 'application/json' }), name);
    setStatus('Downloaded ' + name);
}

async function exportPng() {
    if (!doc.root) return;
    const blob = await view.exportPng(2);
    const base = (doc.name || 'cladogram').replace(/\.json$/i, '');
    download(blob, base + '.png');
    setStatus(`Exported ${base}.png`);
}

async function newDoc() {
    const r = await openEditDialog({ title: 'New cladogram', name: 'Life', withInfo: false });
    if (!r) return;
    fileHandle = null;
    doc.load(makeNode(r.name), 'untitled.json');
    view.selId = doc.root.id;
    setStatus('New cladogram. Select the root and press Add child to grow it');
    refresh();
}

// Drag and drop a file anywhere on the page.
let dragDepth = 0;
addEventListener('dragenter', e => { if (e.dataTransfer?.types.includes('Files')) { ++dragDepth; $('dropZone').hidden = false; } });
addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('dropZone').hidden = true; } });
addEventListener('dragover', e => e.preventDefault());
addEventListener('drop', async e => {
    e.preventDefault();
    dragDepth = 0;
    $('dropZone').hidden = true;
    const item = [...(e.dataTransfer?.items || [])].find(i => i.kind === 'file');
    if (!item) return;
    const h = item.getAsFileSystemHandle ? await item.getAsFileSystemHandle().catch(() => null) : null;
    const f = item.getAsFile();
    if (!f) return;
    if (loadJsonText(await f.text(), f.name)) {
        fileHandle = h && h.kind === 'file' ? h : null;
        setStatus(fileHandle ? `Opened ${f.name}. Save writes back to it` : `Opened ${f.name}`);
    }
});

// ---------------------------------------------------------------- view settings
const prefs = Object.assign({ style: STANDARD, beside: false, theme: matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark' },
                            store.get(PREFS_KEY) || {});
function savePrefs() { store.set(PREFS_KEY, prefs); }

function applyTheme(name) {
    prefs.theme = name;
    document.documentElement.dataset.theme = name;
    document.querySelector('meta[name="theme-color"]').content = name === 'dark' ? '#0e111a' : '#f6f7fa';
    view.theme = THEMES[name];
    view.draw();
    savePrefs();
}
function setStyle(st) {
    if (view.lay.style === st) return;
    prefs.style = st;
    view.lay.style = st;
    view.relayout(false);
    view.fitView();
    savePrefs();
    refresh();
}
function setBeside(on) { prefs.beside = on; view.labelsBeside = on; view.draw(); savePrefs(); }
function tune(f) {
    f(view.lay);
    if (!view.disks()) view.relayout(true);
    updateHud();
}

// ---------------------------------------------------------------- menus
function cladeItems() {
    const n = sel();
    return ACTIONS.flatMap(a => [
        ...(a.id === 'cut' || a.id === 'delete' ? [{ sep: true }] : []),
        { label: labelOf(a, n), key: a.key, run: a.run, disabled: !a.ok(n), danger: a.danger },
    ]);
}
function canvasItems() {
    return [
        { label: 'Fit whole tree', key: 'Home', run: () => view.fitView() },
        { label: 'Zoom in', run: () => view.zoomAt(1.6) },
        { label: 'Zoom out', run: () => view.zoomAt(1 / 1.6) },
    ];
}
const MENUS = {
    File: () => [
        { label: 'New cladogram…', run: newDoc },
        { label: 'Open file…', key: 'Ctrl+O', run: openFile },
        { section: 'Samples' },
        ...SAMPLES.map(s => ({ label: s.title, run: () => openSample(s) })),
        { sep: true },
        { label: fileHandle ? 'Save' : 'Save / download', key: 'Ctrl+S', run: save, disabled: !doc.root },
        { label: 'Save as…', key: 'Ctrl+Shift+S', run: saveAs, disabled: !doc.root },
        { label: 'Export view as PNG', key: 'Ctrl+E', run: exportPng, disabled: !doc.root },
    ],
    Edit: () => [
        { label: 'Undo', key: 'Ctrl+Z', run: undo, disabled: !doc.undoStack.length },
        { label: 'Redo', key: 'Ctrl+Shift+Z', run: redo, disabled: !doc.redoStack.length },
        { sep: true },
        ...cladeItems(),
        { sep: true },
        { label: 'Find clade…', key: 'Ctrl+F', run: focusSearch },
    ],
    View: () => [
        { label: 'Standard layout', key: '1', checked: !view.disks(), run: () => setStyle(STANDARD) },
        { label: 'Disks layout', key: '2', checked: view.disks(), run: () => setStyle(DISKS) },
        { sep: true },
        { label: 'Labels over nodes', checked: !view.labelsBeside, run: () => setBeside(false) },
        { label: 'Labels beside nodes', key: 'L', checked: view.labelsBeside, run: () => setBeside(true) },
        { sep: true },
        { label: 'Dark mode', key: 'D', checked: prefs.theme === 'dark', run: () => applyTheme(prefs.theme === 'dark' ? 'light' : 'dark') },
        { sep: true },
        { label: 'Fit whole tree', key: 'Home', run: () => view.fitView() },
        { label: 'Reset branch spread, shrink and curl', disabled: view.disks(),
          run: () => tune(l => { l.spread = DEFAULT_SPREAD; l.shrink = DEFAULT_SHRINK; l.curl = DEFAULT_CURL; }) },
    ],
    Help: () => [
        { label: 'Controls and shortcuts', key: '?', run: () => $('helpDlg').showModal() },
        { label: 'TreeFern on GitHub', run: () => window.open('https://github.com/DocRountree/treefern', '_blank', 'noopener') },
        { label: 'Download the desktop app', run: () => window.open('https://github.com/DocRountree/treefern/releases/latest', '_blank', 'noopener') },
    ],
};

let menuOwner = null;
function showMenu(items, at, owner = null) {
    const pop = $('menuPop');
    pop.innerHTML = '';
    for (const it of items) {
        if (it.sep) { pop.append(document.createElement('hr')); continue; }
        if (it.section) { const d = document.createElement('div'); d.className = 'sec'; d.textContent = it.section; pop.append(d); continue; }
        const b = document.createElement('button');
        b.setAttribute('role', 'menuitem');
        b.disabled = !!it.disabled;
        if (it.danger) b.classList.add('danger');
        const ck = document.createElement('span');
        ck.className = 'check';
        ck.textContent = it.checked ? '✓' : '';
        const lb = document.createElement('span');
        lb.textContent = it.label;
        b.append(ck, lb);
        if (it.key && !coarse.matches) { const k = document.createElement('span'); k.className = 'key'; k.textContent = it.key; b.append(k); }
        b.addEventListener('click', () => { hideMenu(); it.run(); });
        pop.append(b);
    }
    pop.hidden = false;
    const r = pop.getBoundingClientRect();
    const x = Math.min(at.x, innerWidth - r.width - 8), y = at.y + r.height > innerHeight - 8 ? Math.max(8, innerHeight - r.height - 8) : at.y;
    pop.style.left = Math.max(8, x) + 'px';
    pop.style.top = y + 'px';
    menuOwner?.setAttribute('aria-expanded', 'false');
    menuOwner = owner;
    owner?.setAttribute('aria-expanded', 'true');
}
function hideMenu() {
    $('menuPop').hidden = true;
    menuOwner?.setAttribute('aria-expanded', 'false');
    menuOwner = null;
}
addEventListener('pointerdown', e => {
    if (!$('menuPop').hidden && !$('menuPop').contains(e.target) && !e.target.closest('.menus button, #menuBtn')) hideMenu();
}, true);

function buildMenuBar() {
    const nav = $('menus');
    for (const name of Object.keys(MENUS)) {
        const b = document.createElement('button');
        b.textContent = name;
        b.setAttribute('aria-haspopup', 'menu');
        b.setAttribute('aria-expanded', 'false');
        b.addEventListener('click', () => {
            if (menuOwner === b) return hideMenu();
            const r = b.getBoundingClientRect();
            showMenu(MENUS[name](), { x: r.left, y: r.bottom + 4 }, b);
        });
        b.addEventListener('pointerenter', () => {  // slide between open menus
            if (menuOwner && menuOwner !== b && menuOwner.parentNode === nav) b.click();
        });
        nav.append(b);
    }
    $('menuBtn').addEventListener('click', () => {
        if (menuOwner === $('menuBtn')) return hideMenu();
        const items = Object.entries(MENUS).flatMap(([name, f], i) =>
            [...(i ? [{ sep: true }] : []), { section: name }, ...f().filter(it => !(name === 'Edit' && it.key === 'Ctrl+F'))]);
        const r = $('menuBtn').getBoundingClientRect();
        showMenu(items, { x: r.right, y: r.bottom + 4 }, $('menuBtn'));
    });
}

// ---------------------------------------------------------------- search
function focusSearch() {
    if (narrow.matches) $('panel').classList.add('expanded');
    $('search').focus();
    $('search').select();
}

let hitsShown = [], cur = 0;
// Case-insensitive substring match, ranked exact > prefix > word start > anywhere.
function runSearch() {
    const ul = $('results'), q = $('search').value.trim().toLowerCase();
    ul.innerHTML = '';
    hitsShown = [];
    if (!q || !doc.root) { ul.hidden = true; return; }
    const hits = [];
    walk(doc.root, n => {
        const name = n.name.toLowerCase(), i = name.indexOf(q);
        if (i < 0) return;
        hits.push({ rank: name === q ? 0 : i === 0 ? 1 : !/[\p{L}\p{N}]/u.test(name[i - 1]) ? 2 : 3, n });
    });
    hits.sort((a, b) => a.rank - b.rank || a.n.name.length - b.n.name.length || a.n.name.toLowerCase().localeCompare(b.n.name.toLowerCase()));
    const row = (text, sub, n) => {
        const li = document.createElement('li');
        li.textContent = text;
        if (sub) { const s = document.createElement('span'); s.className = 'in'; s.textContent = sub; li.append(s); }
        if (n) { li.addEventListener('click', () => choose(n)); hitsShown.push({ li, n }); }
        else li.className = 'inert';
        ul.append(li);
    };
    if (!hits.length) row('(no matches)');
    for (const h of hits.slice(0, MAX_SEARCH)) row(h.n.name, h.n.parent ? 'in ' + h.n.parent.name : '', h.n);
    if (hits.length > MAX_SEARCH) row(`… ${hits.length - MAX_SEARCH} more. Refine the search`);
    cur = 0;
    markCur();
    ul.hidden = false;
}
function markCur() {
    hitsShown.forEach((h, i) => h.li.classList.toggle('cur', i === cur));
    hitsShown[cur]?.li.scrollIntoView({ block: 'nearest' });
}
function choose(n) {
    jumpTo(n.id);
    $('search').blur();
    if (narrow.matches) { $('results').hidden = true; $('panel').classList.remove('expanded'); }
}
$('search').addEventListener('input', runSearch);
$('search').addEventListener('keydown', e => {
    if (e.key === 'ArrowDown' && hitsShown.length) { cur = Math.min(hitsShown.length - 1, cur + 1); markCur(); e.preventDefault(); }
    else if (e.key === 'ArrowUp' && hitsShown.length) { cur = Math.max(0, cur - 1); markCur(); e.preventDefault(); }
    else if (e.key === 'Enter') { if (hitsShown[cur]) choose(hitsShown[cur].n); e.preventDefault(); }
    else if (e.key === 'Escape') {
        if ($('search').value) { $('search').value = ''; runSearch(); }
        else $('tree').focus();
        e.preventDefault();
    }
});

// ---------------------------------------------------------------- keyboard
addEventListener('keydown', e => {
    if (document.querySelector('dialog[open]')) return;
    const t = e.target, typing = t.closest && t.closest('input, textarea, [contenteditable]');
    const mod = e.ctrlKey || e.metaKey, k = e.key;
    if (!$('menuPop').hidden && k === 'Escape') { hideMenu(); e.preventDefault(); return; }
    if (mod) {
        const lk = k.toLowerCase();
        const run = {
            z: e.shiftKey ? redo : undo, y: redo,
            f: focusSearch, o: openFile, e: exportPng,
            s: e.shiftKey ? saveAs : save,
        }[lk];
        if (run && !(typing && (lk === 'z' || lk === 'y'))) { e.preventDefault(); run(); }
        return;
    }
    if (typing || e.altKey) return;
    const plain = {
        a: actAdd, e: actEdit, r: actEdit, c: actCollapse,
        f: () => { const n = sel(); if (n) view.focusOn(n); },
        x: actCut, v: actPaste, Delete: actDelete, Backspace: actDelete,
        Escape: () => {
            if (view.cutId) { view.cutId = ''; setStatus('Cut cancelled'); } else view.selId = '';
            refresh();
        },
        ArrowUp: () => navigate('daughter'), ArrowDown: () => navigate('mother'),
        ArrowLeft: () => navigate('left'), ArrowRight: () => navigate('right'),
        Home: () => view.fitView(),
        1: () => setStyle(STANDARD), 2: () => setStyle(DISKS),
        l: () => setBeside(!view.labelsBeside),
        d: () => applyTheme(prefs.theme === 'dark' ? 'light' : 'dark'),
        '/': focusSearch, '?': () => $('helpDlg').showModal(),
        '+': () => tune(l => { l.spread = Math.min(340 * DEG, l.spread + 10 * DEG); }),
        '=': () => tune(l => { l.spread = Math.min(340 * DEG, l.spread + 10 * DEG); }),
        '-': () => tune(l => { l.spread = Math.max(20 * DEG, l.spread - 10 * DEG); }),
        '[': () => tune(l => { l.shrink = Math.max(0.4, l.shrink - 0.02); }),
        ']': () => tune(l => { l.shrink = Math.min(0.9, l.shrink + 0.02); }),
        ',': () => tune(l => { l.curl = Math.max(-1, l.curl - 0.05); }),
        '.': () => tune(l => { l.curl = Math.min(1, l.curl + 0.05); }),
    }[k.length === 1 ? k.toLowerCase() : k];
    if (!plain) return;
    e.preventDefault();
    plain();
});

// ---------------------------------------------------------------- toolbar
$('undoBtn').addEventListener('click', undo);
$('redoBtn').addEventListener('click', redo);
$('fitBtn').addEventListener('click', () => view.fitView());
$('zoomIn').addEventListener('click', () => view.zoomAt(1.6));
$('zoomOut').addEventListener('click', () => view.zoomAt(1 / 1.6));
narrow.addEventListener('change', () => { refresh(); });

// ---------------------------------------------------------------- start
buildActions();
buildMenuBar();
if (CHANNEL) {
    const b = document.createElement('a');
    b.className = 'channel';
    b.href = '../';
    b.textContent = CHANNEL;
    b.title = 'Development build. Go to the stable version';
    document.querySelector('.brand').after(b);
}
view.lay.style = prefs.style === DISKS ? DISKS : STANDARD;
view.labelsBeside = !!prefs.beside;
applyTheme(prefs.theme === 'light' ? 'light' : 'dark');

(async () => {
    const wanted = new URLSearchParams(location.search).get('sample');
    const sample = SAMPLES.find(s => s.file.replace(/\.json$/, '') === wanted);
    const session = store.get(SESSION_KEY);
    if (sample) await openSample(sample);
    else if (session && session.json && loadJsonText(JSON.stringify(session.json), session.name || 'cladogram.json'))
        setStatus('Restored your last session');
    else await openSample(SAMPLES[0]);
    refresh();
})();
