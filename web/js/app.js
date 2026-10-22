// TreeFern for the web: wires the document, the tree view and the page UI.
import { Doc, fromJson, toJson, makeNode, walk, depth, countDesc, contains } from './model.js';
import { TreeView, THEMES } from './view.js';
import { STANDARD, STYLES, STYLE_NAMES, DEFAULT_SPREAD, DEFAULT_SHRINK, DEFAULT_CURL } from './layout.js';
import { DEG, C, clamp } from './geom.js';

let SAMPLES = [];  // [{file, title}], listed from samples/ when the site is built (see build-site.sh)
const MAX_SEARCH = 200, MAX_RECENT = 8;
const CONFIRM_DELETE_OVER = 30;      // ask before deleting more clades than this
const MAX_SVG_CLADES = 20000;        // larger SVGs bring most viewers to a halt
const SLOW_EXPORT_CLADES = 50000;
const MAX_RECENT_TEXT = 5e6;         // files opened without a handle are kept in the browser up to this size
const SWATCHES = [['Red', '#e5534b'], ['Orange', '#e8913a'], ['Yellow', '#d9c13b'], ['Green', '#57ab5a'], ['Teal', '#39b5a5'],
                  ['Blue', '#4f8fe0'], ['Purple', '#9a6fe0'], ['Pink', '#e06fb4'], ['Brown', '#a0704a'], ['Grey', '#8a8f99']];
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
    hover: (id, x, y) => hover(id, x, y),
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

// ---------------------------------------------------------------- hover notes
// A clade's info appears beside the pointer after a short rest on it.
function hover(id, x, y) {
    hideTip();
    const n = doc.find(id);
    if (n && n.info.trim()) hover.timer = setTimeout(() => showTip(n, x, y), 450);
}
function showTip(n, x, y) {
    const t = $('tip'), b = document.createElement('b');
    b.textContent = n.name;
    const info = n.info.trim();
    t.replaceChildren(b, info.length > 600 ? info.slice(0, 600).trimEnd() + '…' : info);
    t.hidden = false;
    const r = t.getBoundingClientRect();
    t.style.left = Math.max(8, Math.min(x + 14, innerWidth - r.width - 8)) + 'px';
    t.style.top = (y + 18 + r.height > innerHeight - 8 ? Math.max(8, y - r.height - 10) : y + 18) + 'px';
}
function hideTip() { clearTimeout(hover.timer); $('tip').hidden = true; }
for (const ev of ['pointerdown', 'wheel']) $('tree').addEventListener(ev, hideTip, { passive: true });

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
        if (doc.root && !store.set(SESSION_KEY, { name: doc.name, json: doc.current, recentKey }))
            setStatus('Could not keep a copy in this browser; use Save or Download', true);
        keepRecentText();
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
    const parts = [STYLE_NAMES[L.style], 'zoom ' + view.zoomText(), `${n} nodes`];
    if (L.style === STANDARD && !narrow.matches)
        parts.push(`spread ${Math.round(L.spread / DEG)}°  shrink ${L.shrink.toFixed(2)}  curl ${L.curl.toFixed(2)}`);
    if (cut) parts.push(`cut: ${cut.name} (select a target, then Paste)`);
    if (status) parts.push(status);
    $('hud').textContent = parts.join('   ·   ');
}

// ---------------------------------------------------------------- panel
const ACTIONS = [
    { id: 'add', label: 'Add child', key: 'A / D', run: () => actAdd(), ok: n => !!n },
    { id: 'sister', label: 'Add sister', key: 'S', run: () => actAdd(true), ok: n => !!n && !!n.parent },
    { id: 'edit', label: 'Edit', key: 'E', run: () => actEdit(), ok: n => !!n },
    { id: 'collapse', label: n => n && n.collapsed ? 'Expand' : 'Collapse', key: 'C', run: () => actCollapse(), ok: n => !!n && n.kids.length > 0 },
    { id: 'color', label: 'Colour…', key: 'K', run: () => actColor(), ok: n => !!n },
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
    renderCrumbs(n);
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
    const id = n.id;
    renderPics($('pics'), () => doc.find(id)?.images || [], i => {
        const m = doc.find(id);
        if (!m) return;
        m.images = m.images.filter((_, j) => j !== i);
        doc.commit();
        setStatus(`Removed a picture from '${m.name}'`);
    });
}

// The clades above the selection, root first; long trails fold their middle
// into a "…" that unfolds when clicked.
let crumbsOpen = '';
function renderCrumbs(n) {
    const box = $('crumbs'), path = [];
    for (let a = n.parent; a; a = a.parent) path.unshift(a);
    if (crumbsOpen !== n.id) crumbsOpen = '';
    const shown = crumbsOpen || path.length <= 6 ? path : [path[0], null, ...path.slice(-4)];
    box.replaceChildren();
    shown.forEach((a, i) => {
        if (i) { const s = document.createElement('span'); s.className = 'sep'; s.textContent = '›'; box.append(s); }
        const b = document.createElement('button');
        if (a) { b.textContent = a.name; b.title = 'Go to ' + a.name; b.addEventListener('click', () => goTo(a)); }
        else {
            b.textContent = '…';
            b.title = `Show ${path.length - 5} more`;
            b.addEventListener('click', () => { crumbsOpen = n.id; renderCrumbs(n); });
        }
        box.append(b);
    });
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
let dlgResolve = null, dlgRapid = false, dlgImages = [];
const EDIT_HINT = 'Enter in the name, or Ctrl+Enter anywhere: OK. Esc: cancel.';
const RAPID_HINT = 'Enter or Add: add it and stop. Shift+Enter: add it, then a daughter of it. Ctrl+Tab: add it, then a sister. Tab: to the info. Esc: stop.';
// rapid: adding clades one after another; resolves with next = 'child', 'sister' or 'done'.
function openEditDialog({ title, name = '', info = '', images = [], withInfo = true, rapid = false }) {
    const dlg = $('editDlg');
    dlgRapid = rapid;
    dlgImages = images.slice();
    renderEditPics();
    $('editPicsRow').hidden = !withInfo;
    $('editTitle').textContent = title;
    $('editName').value = name;
    $('editInfo').value = info;
    $('editInfoRow').hidden = !withInfo;
    $('editHint').textContent = rapid ? RAPID_HINT : EDIT_HINT;
    $('editSister').hidden = $('editDaughter').hidden = !rapid;
    $('editOk').textContent = rapid ? 'Add' : 'OK';
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
$('editSister').addEventListener('click', () => finishEdit(true, 'sister'));
$('editDaughter').addEventListener('click', () => finishEdit(true, 'child'));
$('editName').addEventListener('keydown', e => {
    if (!dlgRapid || e.ctrlKey || e.metaKey || e.altKey || e.isComposing) return;
    if (e.key === 'Enter') { e.preventDefault(); finishEdit(true, e.shiftKey ? 'child' : 'done'); }
});
// Ctrl+Tab: add it, then a sister. Browser tabs keep Ctrl+Tab for themselves; an
// installed app's window passes it on. Plain Tab moves on to the info as usual.
$('editDlg').addEventListener('keydown', e => {
    if (dlgRapid && e.key === 'Tab' && e.ctrlKey && !e.shiftKey && !e.altKey) { e.preventDefault(); finishEdit(true, 'sister'); }
});
function finishEdit(ok, next = 'done') {
    const name = $('editName').value.trim(), info = $('editInfo').value;
    if (ok && !name) {
        if (!dlgRapid) { $('editName').focus(); return; }
        ok = false;  // rapid entry: confirming an empty name stops
    }
    $('editDlg').close();
    const r = dlgResolve;
    dlgResolve = null;
    r?.(ok ? { name, info, next, images: dlgImages.slice() } : null);
}
$('editDlg').addEventListener('cancel', e => { e.preventDefault(); finishEdit(false); });
$('editDlg').addEventListener('keydown', e => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); finishEdit(true); }
});

// Dialogs give an external change no chance here, but a node may be gone
// after an undo in another tab; re-look it up by id like the desktop app.
// Adds clades until the user stops: Enter continues with a daughter of the
// clade just added, Tab with a sister of it. Each clade is its own undo step.
// sister: start with a sister of the selection instead of a daughter.
async function actAdd(sister = false) {
    const n = sel();
    if (!n) return;
    if (sister && !n.parent) return setStatus('The root clade cannot have sisters', true);
    let parentId = sister ? n.parent.id : n.id;
    for (;;) {
        let p = doc.find(parentId);
        if (!p) return;
        const r = await openEditDialog({ title: `New clade under '${p.name}'`, rapid: true });
        if (!r || !(p = doc.find(parentId))) return;
        const c = doc.addChild(p, r.name, r.info, r.images);
        view.selId = c.id;
        refresh();
        if (r.next === 'done') return;
        if (r.next === 'child') parentId = c.id;
    }
}

async function actEdit() {
    let n = sel();
    if (!n) return;
    const id = n.id;
    const r = await openEditDialog({ title: 'Edit clade', name: n.name, info: n.info, images: n.images });
    if (!r || !(n = doc.find(id))) return;
    const samePics = r.images.length === n.images.length && r.images.every((s, i) => s === n.images[i]);
    if (r.name === n.name && r.info === n.info && samePics) return;
    n.name = r.name;
    n.info = r.info;
    if (!samePics) n.images = r.images;
    doc.commit();
}

// ---------------------------------------------------------------- pictures
// Pictures are kept in the file with the notes, so they are scaled to at most
// IMAGE_MAX_SIDE px and stored as JPEG, or as PNG when they have transparency.
const IMAGE_MAX_SIDE = 1200, IMAGE_QUALITY = 0.85;
const isImage = f => /^image\/(png|jpeg|gif|webp|bmp)$/.test(f.type);
const imageFiles = dt => [...(dt?.files || [])].filter(isImage);
const dragHasImages = dt => [...(dt?.items || [])].some(i => i.kind === 'file' && i.type.startsWith('image/'));

async function toDataUrl(file) {
    const bmp = await createImageBitmap(file);
    const k = Math.min(1, IMAGE_MAX_SIDE / Math.max(bmp.width, bmp.height));
    const c = document.createElement('canvas');
    c.width = Math.max(1, Math.round(bmp.width * k));
    c.height = Math.max(1, Math.round(bmp.height * k));
    const g = c.getContext('2d');
    g.drawImage(bmp, 0, 0, c.width, c.height);
    bmp.close?.();
    let alpha = false;
    if (file.type !== 'image/jpeg') {
        const px = g.getImageData(0, 0, c.width, c.height).data;
        for (let i = 3; i < px.length; i += 4) if (px[i] < 255) { alpha = true; break; }
    }
    return alpha ? c.toDataURL('image/png') : c.toDataURL('image/jpeg', IMAGE_QUALITY);
}

// The pictures among `files`, converted; unreadable ones are reported and skipped.
async function readPictures(files) {
    const urls = [];
    for (const f of files)
        if (isImage(f)) {
            try { urls.push(await toDataUrl(f)); }
            catch { setStatus(`Cannot read the picture ${f.name}`, true); }
        }
    return urls;
}

async function addPictures(id, files) {
    const urls = await readPictures(files);
    const n = doc.find(id);
    if (!n || !urls.length) return;
    n.images = [...n.images, ...urls];
    doc.commit();
    setStatus(`Added ${urls.length === 1 ? 'a picture' : urls.length + ' pictures'} to '${n.name}'`);
}

// Thumbnails of get() in box; clicking one opens the viewer, which can remove it.
function renderPics(box, get, remove) {
    box.replaceChildren();
    get().forEach((src, i) => {
        const d = document.createElement('div'), img = document.createElement('img');
        d.className = 'pic';
        img.src = src;
        img.alt = `Picture ${i + 1}`;
        img.addEventListener('click', () => showPicture(get, i, remove));
        d.append(img);
        box.append(d);
    });
}

function renderEditPics() {
    const remove = i => { dlgImages.splice(i, 1); renderEditPics(); };
    renderPics($('editPics'), () => dlgImages, remove);
    [...$('editPics').children].forEach((d, i) => {
        const x = document.createElement('button');
        x.type = 'button';
        x.textContent = '×';
        x.title = 'Remove this picture';
        x.addEventListener('click', () => remove(i));
        d.append(x);
    });
}
async function addEditPics(files) {
    dlgImages.push(...await readPictures(files));
    renderEditPics();
}
$('editAddPics').addEventListener('click', () => { $('picInput').onchange = () => addEditPics([...$('picInput').files]); $('picInput').value = ''; $('picInput').click(); });
$('editDlg').addEventListener('paste', e => {
    const files = imageFiles(e.clipboardData);
    if (!files.length) return;
    e.preventDefault();
    addEditPics(files);
});

// Drop targets: the edit box (its pending pictures), the side panel (the
// selected clade) and, in the global handler below, the clade under the pointer.
for (const [el, drop] of [[$('editDlg'), files => addEditPics(files)],
                          [$('panel'), files => { const n = sel(); if (n) addPictures(n.id, files); }]]) {
    el.addEventListener('dragover', e => {
        if (!dragHasImages(e.dataTransfer) || (el === $('panel') && !sel())) return;
        e.preventDefault();
        e.stopPropagation();
        el.classList.add('drop-target');
    });
    el.addEventListener('dragleave', e => { if (!el.contains(e.relatedTarget)) el.classList.remove('drop-target'); });
    el.addEventListener('drop', e => {
        el.classList.remove('drop-target');
        const files = imageFiles(e.dataTransfer);
        if (!files.length) return;
        e.preventDefault();
        e.stopPropagation();
        dragDepth = 0;
        $('dropZone').hidden = true;
        drop(files);
    });
}

let picView = null;  // the viewer's {get, i, remove, back}; back: the edit box it stands in for
function showPicture(get, i, remove) {
    const back = $('editDlg').open;
    picView = { get, i, remove, back };
    const d = $('picDlg');
    if (back) {  // take the edit box's place and size; it comes back when the viewer closes
        const r = $('editDlg').getBoundingClientRect();
        d.style.width = r.width + 'px';
        d.style.height = r.height + 'px';
        $('editDlg').close();
    } else d.style.width = d.style.height = '';
    drawPicture();
    if (!d.open) d.showModal();
}
$('picDlg').addEventListener('close', () => {
    if (!picView?.back) return;
    picView.back = false;
    $('editDlg').showModal();
    $('editInfo').focus();
});
function drawPicture() {
    const list = picView.get();
    if (!list.length) return $('picDlg').close();
    picView.i = clamp(picView.i, 0, list.length - 1);
    $('picBig').src = list[picView.i];
    $('picCaption').textContent = `Picture ${picView.i + 1} of ${list.length}`;
    $('picPrev').disabled = picView.i === 0;
    $('picNext').disabled = picView.i === list.length - 1;
}
$('picPrev').addEventListener('click', () => { --picView.i; drawPicture(); });
$('picNext').addEventListener('click', () => { ++picView.i; drawPicture(); });
$('picRemove').addEventListener('click', () => { picView.remove(picView.i); drawPicture(); });
$('picDlg').addEventListener('keydown', e => {
    if (e.key === 'ArrowLeft' && !$('picPrev').disabled) { $('picPrev').click(); e.preventDefault(); }
    if (e.key === 'ArrowRight' && !$('picNext').disabled) { $('picNext').click(); e.preventDefault(); }
});

function actCollapse() {
    const n = sel();
    if (!n || !n.kids.length) return;
    n.collapsed = !n.collapsed;
    doc.commit();
}

async function actDelete() {
    let n = sel();
    if (!n) return;
    if (!n.parent) return setStatus('Cannot delete the root clade', true);
    const id = n.id, inside = countDesc(n);
    if (inside + 1 > CONFIRM_DELETE_OVER &&
        (!await askConfirm(`Delete '${n.name}'?`, `This deletes ${inside + 1} clades: '${n.name}' and the ${inside} inside it. You can undo it with Ctrl+Z.`)
         || !(n = doc.find(id)))) return;
    const parentId = n.parent ? n.parent.id : '';
    const name = n.name;
    const err = doc.removeSubtree(n);
    if (err) return setStatus(err, true);
    view.selId = parentId;
    setStatus(`Deleted '${name}'. Undo with Ctrl+Z or the undo button`);
    refresh();
}

let confirmResolve = null;
function askConfirm(title, text, okLabel = 'Delete') {
    $('confirmTitle').textContent = title;
    $('confirmText').textContent = text;
    $('confirmOk').textContent = okLabel;
    $('confirmDlg').showModal();
    $('confirmCancel').focus();
    return new Promise(res => { confirmResolve = res; });
}
function finishConfirm(ok) {
    $('confirmDlg').close();
    const r = confirmResolve;
    confirmResolve = null;
    r?.(ok);
}
$('confirmForm').addEventListener('submit', e => { e.preventDefault(); finishConfirm(true); });
$('confirmCancel').addEventListener('click', () => finishConfirm(false));
$('confirmDlg').addEventListener('cancel', e => { e.preventDefault(); finishConfirm(false); });

// Colour: a swatch menu by the panel's Colour button (or where the last menu was).
function actColor() {
    const n = sel();
    if (!n) return;
    const id = n.id;
    const set = c => {
        const m = doc.find(id);
        if (!m || m.color === c) return;
        m.color = c;
        doc.commit();
        setStatus(c ? `Coloured '${m.name}'` : `'${m.name}' uses ${m.parent ? "its mother clade's" : 'the default'} colours`);
    };
    const b = $('actions').querySelector('[data-act="color"]'), r = b.getBoundingClientRect();
    const at = performance.now() - lastMenuRun.t < 400 ? lastMenuRun.at
             : r.width && r.top < innerHeight ? { x: r.left, y: r.bottom + 4 } : { x: innerWidth / 2 - 110, y: innerHeight / 4 };
    const items = SWATCHES.map(([name, c]) => ({ label: name, swatch: c, checked: n.color === c, run: () => set(c) }));
    items.push({ sep: true },
               { label: 'Custom colour…', run: () => {
                   const inp = $('colorInput');
                   inp.style.left = at.x + 'px';
                   inp.style.top = at.y + 'px';
                   inp.value = n.color || '#4f8fe0';
                   inp.onchange = () => set(inp.value.toLowerCase());
                   try { inp.showPicker(); } catch { inp.click(); }
               } },
               { label: n.parent ? 'Same as mother clade' : 'Default colours', checked: !n.color, run: () => set('') });
    showMenu(items, at);
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

// ---------------------------------------------------------------- recent files
// Kept in IndexedDB: a file handle where the browser gives one (Chromium), so
// reopening edits the file itself; otherwise the file's text, which follows
// later edits so the entry reopens as last edited.
const DB_NAME = CHANNEL ? `treefern.${CHANNEL}` : 'treefern';
const idb = (() => {
    let db = null;
    const open = () => db ||= new Promise((res, rej) => {
        const r = indexedDB.open(DB_NAME, 1);
        r.onupgradeneeded = () => r.result.createObjectStore('recent', { keyPath: 'key' });
        r.onsuccess = () => res(r.result);
        r.onerror = () => rej(r.error);
    });
    const run = async (mode, f) => {
        const d = await open();
        return new Promise((res, rej) => {
            const t = d.transaction('recent', mode), req = f(t.objectStore('recent'));
            t.oncomplete = () => res(req.result);
            t.onerror = t.onabort = () => rej(t.error);
        });
    };
    return {
        all: () => run('readonly', st => st.getAll()),
        put: v => run('readwrite', st => st.put(v)),
        del: k => run('readwrite', st => st.delete(k)),
        clear: () => run('readwrite', st => st.clear()),
    };
})();
let recent = [], recentKey = '';  // newest first; recentKey: the entry the open document came from

async function loadRecent() {
    try { recent = (await idb.all()).sort((a, b) => b.time - a.time); } catch { recent = []; }
}

async function remember(name, handle, text) {
    recentKey = '';
    if (!handle && (text === undefined || text.length > MAX_RECENT_TEXT)) return;
    try {
        let key = handle ? '' : 't:' + name;
        if (handle)
            for (const e of recent)
                if (e.handle && await e.handle.isSameEntry(handle).catch(() => false)) { key = e.key; break; }
        key ||= 'h:' + Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
        await idb.put(handle ? { key, name, handle, time: Date.now() } : { key, name, text, time: Date.now() });
        recentKey = key;
        await loadRecent();
        for (const e of recent.slice(MAX_RECENT)) await idb.del(e.key);
        recent = recent.slice(0, MAX_RECENT);
    } catch { /* no IndexedDB: no recent list */ }
}

function keepRecentText() {
    const e = recent.find(x => x.key === recentKey);
    if (!e || e.handle || !doc.root) return;
    const text = jsonText();
    if (text.length > MAX_RECENT_TEXT) return;
    e.text = text;
    idb.put(e).catch(() => {});
}

async function openRecent(e) {
    try {
        if (e.handle) {
            const opts = { mode: 'readwrite' };
            if (await e.handle.queryPermission(opts) !== 'granted' && await e.handle.requestPermission(opts) !== 'granted')
                return setStatus(`No permission to open ${e.name}`, true);
            const f = await e.handle.getFile();
            if (loadJsonText(await f.text(), f.name)) {
                fileHandle = e.handle;
                await remember(f.name, e.handle);
                setStatus(`Opened ${f.name}. Edits are saved to the file`);
            }
        } else if (loadJsonText(e.text, e.name)) {
            fileHandle = null;
            await remember(e.name, null, e.text);
            setStatus(`Opened ${e.name} as last edited in this browser`);
        }
    } catch (err) {
        setStatus(`Cannot open ${e.name}: ${err.message}. Removed it from the recent list`, true);
        await idb.del(e.key).catch(() => {});
        await loadRecent();
    }
}

async function clearRecent() {
    try { await idb.clear(); } catch { /* nothing kept anyway */ }
    recent = [];
    recentKey = '';
    setStatus('Cleared the recent files');
}

// ---------------------------------------------------------------- files
function loadJsonText(text, name) {
    let root;
    try { root = fromJson(JSON.parse(text)); }
    catch (e) { setStatus(`Cannot open ${name}: ${e.message}`, true); return false; }
    doc.load(root, name);
    return true;
}

// Both are revalidated with the server, so a newly published sample shows at once.
async function loadSamples() {
    try {
        const r = await fetch('samples/index.json', { cache: 'no-cache' });
        if (r.ok) SAMPLES = (await r.json()).filter(s => s && typeof s.file === 'string' && typeof s.title === 'string');
    } catch { /* no list: the menu has no samples */ }
}

async function openSample(s, startup = false) {
    try {
        const r = await fetch('samples/' + s.file, { cache: 'no-cache' });
        if (!r.ok) throw new Error(r.status + ' ' + r.statusText);
        if (startup && launched) return;  // a file arrived through "open with" meanwhile
        if (loadJsonText(await r.text(), s.file)) {
            fileHandle = null;
            recentKey = '';
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
                await remember(f.name, h);
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
    const text = await f.text();
    if (loadJsonText(text, f.name)) {
        fileHandle = null;
        await remember(f.name, null, text);
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
                await remember(h.name, h);
                saveSession();
                refresh();
            }
        } catch (e) { if (e.name !== 'AbortError') setStatus('Cannot save: ' + e.message, true); }
        return;
    }
    download(new Blob([jsonText()], { type: 'application/json' }), name);
    setStatus('Downloaded ' + name);
}

// ---------------------------------------------------------------- export
// The view or the whole tree, as PNG (copied or saved) or SVG. Image sizes are
// capped in TreeView; SVGs too large for viewers to open are refused here.
function exportOpts() {
    const v = name => document.querySelector(`input[name="${name}"]:checked`).value;
    return { area: v('exArea'), format: v('exFormat'), scale: +v('exScale'), transparent: $('exTransparent').checked };
}
function updateExportInfo() {
    const o = exportOpts(), whole = o.area === 'whole', svg = o.format === 'svg';
    const n = view.exportCount(whole);
    $('exScaleRow').disabled = svg;
    $('exCopy').disabled = svg || !window.ClipboardItem || !navigator.clipboard?.write;
    let text, ok = true;
    if (svg && n > MAX_SVG_CLADES) {
        text = `This would put ${n.toLocaleString()} clades in one SVG; most programs cannot open SVGs that large ` +
               `(the limit is ${MAX_SVG_CLADES.toLocaleString()}). Collapse some clades, zoom in, or export a PNG.`;
        ok = false;
    } else if (svg) {
        const { w, h } = view.exportSize(whole, 1);
        text = `${w} × ${h}, ${n.toLocaleString()} clades.`;
    } else {
        const { pw, ph, factor } = view.exportSize(whole, o.scale);
        text = `${pw.toLocaleString()} × ${ph.toLocaleString()} pixels` +
               (factor < o.scale - 1e-9 ? ', reduced to stay within what browsers can draw.' : '.');
        if (n > SLOW_EXPORT_CLADES) text += ` Drawing ${n.toLocaleString()} clades may take a few seconds.`;
    }
    $('exInfo').textContent = text;
    $('exSave').disabled = !ok;
}
function openExport() {
    if (!doc.root) return;
    const o = Object.assign({ area: 'view', format: 'png', scale: 2, transparent: false }, prefs.export || {});
    for (const [name, val] of [['exArea', o.area], ['exFormat', o.format], ['exScale', String(o.scale)]]) {
        const r = document.querySelector(`input[name="${name}"][value="${val}"]`);
        if (r) r.checked = true;
    }
    $('exTransparent').checked = !!o.transparent;
    updateExportInfo();
    $('exportDlg').showModal();
}
$('exportForm').addEventListener('change', updateExportInfo);
$('exCancel').addEventListener('click', () => $('exportDlg').close());
$('exportForm').addEventListener('submit', async e => {
    e.preventDefault();
    const o = exportOpts(), whole = o.area === 'whole';
    prefs.export = o;
    savePrefs();
    $('exportDlg').close();
    const base = (doc.name || 'cladogram').replace(/\.json$/i, '');
    setStatus('Exporting…');
    await new Promise(r => setTimeout(r, 30));  // let the dialog close first
    try {
        const name = `${base}.${o.format}`;
        download(o.format === 'svg' ? view.exportSvg({ whole, transparent: o.transparent, title: base })
                                    : await view.exportPng({ whole, factor: o.scale, transparent: o.transparent }), name);
        setStatus('Exported ' + name);
    } catch (err) { setStatus('Cannot export: ' + err.message, true); }
});
// The clipboard write starts inside the click (Safari requires it), with the image still rendering.
$('exCopy').addEventListener('click', () => {
    const o = exportOpts();
    prefs.export = o;
    savePrefs();
    $('exportDlg').close();
    const png = view.exportPng({ whole: o.area === 'whole', factor: o.scale, transparent: o.transparent });
    navigator.clipboard.write([new ClipboardItem({ 'image/png': png })])
        .then(() => setStatus('Copied the image to the clipboard'), err => setStatus('Cannot copy the image: ' + err.message, true));
});

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
// Pictures dragged over the tree highlight the clade they would be added to.
const treePoint = e => { const r = $('tree').getBoundingClientRect(); return C(e.clientX - r.left, e.clientY - r.top); };
const overTree = e => $('tree').contains(e.target);
function dragHover(id) { if (view.hoverId !== id) { view.hoverId = id; view.draw(); } }
addEventListener('dragenter', e => {
    if (!e.dataTransfer?.types.includes('Files')) return;
    ++dragDepth;
    if (!dragHasImages(e.dataTransfer)) $('dropZone').hidden = false;
});
addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; $('dropZone').hidden = true; dragHover(''); } });
addEventListener('dragover', e => {
    e.preventDefault();
    if (dragHasImages(e.dataTransfer)) dragHover(overTree(e) ? view.hitTest(treePoint(e), 14)?.id || '' : '');
});
addEventListener('drop', async e => {
    e.preventDefault();
    dragDepth = 0;
    $('dropZone').hidden = true;
    dragHover('');
    const pics = imageFiles(e.dataTransfer);
    if (pics.length) {
        const n = (overTree(e) && view.hitTest(treePoint(e), 14)) || sel();
        if (n) addPictures(n.id, pics);
        else setStatus('Drop pictures onto a clade to add them to its notes', true);
        return;
    }
    const item = [...(e.dataTransfer?.items || [])].find(i => i.kind === 'file');
    if (!item) return;
    const h = item.getAsFileSystemHandle ? await item.getAsFileSystemHandle().catch(() => null) : null;
    const f = item.getAsFile();
    if (!f) return;
    const text = await f.text();
    if (loadJsonText(text, f.name)) {
        fileHandle = h && h.kind === 'file' ? h : null;
        await remember(f.name, fileHandle, text);
        setStatus(fileHandle ? `Opened ${f.name}. Save writes back to it` : `Opened ${f.name}`);
    }
});

// "Open with" from the operating system, once TreeFern is installed as an app.
let launched = false;
if ('launchQueue' in window)
    launchQueue.setConsumer(async params => {
        const h = params.files?.[0];
        if (!h) return;
        launched = true;
        try {
            const f = await h.getFile();
            if (loadJsonText(await f.text(), f.name)) {
                fileHandle = h;
                await remember(f.name, h);
                setStatus(`Opened ${f.name}. Edits are saved to the file`);
            }
        } catch (e) { setStatus('Cannot open the file: ' + e.message, true); }
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
    if (view.lay.style === STANDARD) view.relayout(true);
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
        ...(recent.length ? [{ section: 'Recent' },
                             ...recent.map(e => ({ label: e.name, run: () => openRecent(e) })),
                             { label: 'Clear recent files', run: clearRecent }] : []),
        ...(SAMPLES.length ? [{ section: 'Samples' }] : []),
        ...SAMPLES.map(s => ({ label: s.title, run: () => openSample(s) })),
        { sep: true },
        { label: fileHandle ? 'Save' : 'Save / download', key: 'Ctrl+S', run: save, disabled: !doc.root },
        { label: 'Save as…', key: 'Ctrl+Shift+S', run: saveAs, disabled: !doc.root },
        { label: 'Export image…', key: 'Ctrl+E', run: openExport, disabled: !doc.root },
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
        ...STYLES.map((st, i) => ({ label: STYLE_NAMES[st] + ' layout', key: String(i + 1), checked: view.lay.style === st, run: () => setStyle(st) })),
        { sep: true },
        { label: 'Labels over nodes', checked: !view.labelsBeside, run: () => setBeside(false) },
        { label: 'Labels beside nodes', key: 'L', checked: view.labelsBeside, run: () => setBeside(true) },
        { sep: true },
        { label: 'Dark mode', key: 'Shift+D', checked: prefs.theme === 'dark', run: () => applyTheme(prefs.theme === 'dark' ? 'light' : 'dark') },
        { sep: true },
        { label: 'Fit whole tree', key: 'Home', run: () => view.fitView() },
        { label: 'Zoom in', run: () => view.zoomAt(1.6) },
        { label: 'Zoom out', run: () => view.zoomAt(1 / 1.6) },
        { label: 'Reset branch spread, shrink and curl', disabled: view.lay.style !== STANDARD,
          run: () => tune(l => { l.spread = DEFAULT_SPREAD; l.shrink = DEFAULT_SHRINK; l.curl = DEFAULT_CURL; }) },
    ],
    Help: () => [
        { label: 'Command palette…', key: 'Ctrl+K', run: openPalette },
        { label: 'Controls and shortcuts', key: '?', run: () => $('helpDlg').showModal() },
        { label: 'TreeFern on GitHub', run: () => window.open('https://github.com/DocRountree/treefern', '_blank', 'noopener') },
        { label: 'Download the desktop app', run: () => window.open('https://github.com/DocRountree/treefern/releases/latest', '_blank', 'noopener') },
    ],
};

let menuOwner = null;
const lastMenuRun = { t: -1e9, at: { x: 0, y: 0 } };  // so a follow-up menu opens in the same place
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
        b.append(ck);
        if (it.swatch) { const sw = document.createElement('span'); sw.className = 'swatch'; sw.style.background = it.swatch; b.append(sw); }
        b.append(lb);
        if (it.key && !coarse.matches) { const k = document.createElement('span'); k.className = 'key'; k.textContent = it.key; b.append(k); }
        b.addEventListener('click', () => { hideMenu(); lastMenuRun.t = performance.now(); lastMenuRun.at = at; it.run(); });
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

// ---------------------------------------------------------------- command palette
// Every menu command, found by typing any part of its name.
let palItems = [], palShown = [], palCur = 0;
function openPalette() {
    hideMenu();
    palItems = [];
    const seen = new Set();
    for (const [menu, f] of Object.entries(MENUS)) {
        let group = menu;
        for (const it of f()) {
            if (it.sep) { group = menu; continue; }
            if (it.section) { group = `${menu} › ${it.section}`; continue; }
            if (it.run === openPalette || seen.has(it.label)) continue;
            seen.add(it.label);
            palItems.push({ ...it, group });
        }
    }
    $('paletteInput').value = '';
    runPalette();
    $('paletteDlg').showModal();
    $('paletteInput').focus();
}
function runPalette() {
    const words = $('paletteInput').value.trim().toLowerCase().split(/\s+/).filter(Boolean);
    const scored = [];
    palItems.forEach((it, i) => {
        const label = it.label.toLowerCase(), hay = label + ' ' + it.group.toLowerCase();
        if (!words.every(w => hay.includes(w))) return;
        const q = words.join(' '), at = label.indexOf(q);
        const rank = !q ? 0 : at === 0 ? 0 : at > 0 && !/[\p{L}\p{N}]/u.test(label[at - 1]) ? 1 : at > 0 ? 2 : 3;
        scored.push({ it, key: [it.disabled ? 1 : 0, rank, i] });
    });
    scored.sort((a, b) => a.key[0] - b.key[0] || a.key[1] - b.key[1] || a.key[2] - b.key[2]);
    const ul = $('paletteList');
    ul.replaceChildren();
    palShown = [];
    for (const { it } of scored) {
        const li = document.createElement('li');
        li.setAttribute('role', 'option');
        if (it.key) { const k = document.createElement('span'); k.className = 'key'; k.textContent = it.key; li.append(k); }
        li.append((it.checked ? '✓ ' : '') + it.label);
        const g = document.createElement('span');
        g.className = 'grp';
        g.textContent = it.group;
        li.append(g);
        if (it.disabled) li.classList.add('off');
        else { li.addEventListener('click', () => runPaletteItem(it)); }
        palShown.push({ li, it });
        ul.append(li);
    }
    if (!palShown.length) { const li = document.createElement('li'); li.className = 'inert'; li.textContent = '(no matching commands)'; ul.append(li); }
    palCur = palShown.findIndex(p => !p.it.disabled);
    markPalette();
}
function markPalette() {
    palShown.forEach((p, i) => p.li.classList.toggle('cur', i === palCur));
    palShown[palCur]?.li.scrollIntoView({ block: 'nearest' });
}
function runPaletteItem(it) {
    $('paletteDlg').close();
    lastMenuRun.t = -1e9;
    it.run();
}
$('paletteInput').addEventListener('input', runPalette);
$('paletteInput').addEventListener('keydown', e => {
    const step = d => {
        for (let i = palCur + d; i >= 0 && i < palShown.length; i += d) if (!palShown[i].it.disabled) { palCur = i; break; }
        markPalette();
    };
    if (e.key === 'ArrowDown') { step(1); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { step(-1); e.preventDefault(); }
    else if (e.key === 'Enter') { e.preventDefault(); if (palShown[palCur] && !palShown[palCur].it.disabled) runPaletteItem(palShown[palCur].it); }
});
$('paletteForm').addEventListener('submit', e => e.preventDefault());
$('paletteDlg').addEventListener('click', e => { if (e.target === $('paletteDlg')) $('paletteDlg').close(); });  // backdrop

// ---------------------------------------------------------------- keyboard
addEventListener('keydown', e => {
    hideTip();
    if (document.querySelector('dialog[open]')) return;
    const t = e.target, typing = t.closest && t.closest('input, textarea, [contenteditable]');
    const mod = e.ctrlKey || e.metaKey, k = e.key;
    if (!$('menuPop').hidden && k === 'Escape') { hideMenu(); e.preventDefault(); return; }
    if (mod) {
        const lk = k.toLowerCase();
        const run = {
            z: e.shiftKey ? redo : undo, y: redo,
            f: focusSearch, o: openFile, e: openExport, k: openPalette,
            s: e.shiftKey ? saveAs : save,
        }[lk];
        if (run && !(typing && (lk === 'z' || lk === 'y'))) { e.preventDefault(); run(); }
        return;
    }
    if (typing || e.altKey) return;
    const plain = {
        a: actAdd, s: () => actAdd(true), e: actEdit, r: actEdit, c: actCollapse, k: actColor,
        f: () => { const n = sel(); if (n) view.focusOn(n); },
        x: actCut, v: actPaste, Delete: actDelete, Backspace: actDelete,
        Escape: () => {
            if (view.cutId) { view.cutId = ''; setStatus('Cut cancelled'); } else view.selId = '';
            refresh();
        },
        ArrowUp: () => navigate('daughter'), ArrowDown: () => navigate('mother'),
        ArrowLeft: () => navigate('left'), ArrowRight: () => navigate('right'),
        Home: () => view.fitView(),
        1: () => setStyle(STYLES[0]), 2: () => setStyle(STYLES[1]), 3: () => setStyle(STYLES[2]), 4: () => setStyle(STYLES[3]),
        l: () => setBeside(!view.labelsBeside),
        d: e.shiftKey ? () => applyTheme(prefs.theme === 'dark' ? 'light' : 'dark') : actAdd,  // D: add a daughter
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
view.lay.style = STYLES.includes(prefs.style) ? prefs.style : STANDARD;
view.labelsBeside = !!prefs.beside;
applyTheme(prefs.theme === 'light' ? 'light' : 'dark');

(async () => {
    await loadSamples();
    const wanted = new URLSearchParams(location.search).get('sample');
    const sample = SAMPLES.find(s => s.file.replace(/\.json$/, '') === wanted);
    const session = store.get(SESSION_KEY);
    if (sample) await openSample(sample);
    else if (session && session.json && loadJsonText(JSON.stringify(session.json), session.name || 'cladogram.json')) {
        recentKey = session.recentKey || '';
        setStatus('Restored your last session');
    } else if (!launched && SAMPLES.length) await openSample(SAMPLES[0], true);
    refresh();
    await loadRecent();
})();
