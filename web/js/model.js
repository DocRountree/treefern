// The cladogram tree, its JSON file format, and undo history.
// File format (shared with the desktop app):
//   {"root": id, "nodes": [{id, parent?, name, info, collapsed}, ...]}

export function newId() {
    const a = new Uint8Array(5);
    crypto.getRandomValues(a);
    return Array.from(a, b => b.toString(16).padStart(2, '0')).join('');
}

export function makeNode(name, info = '', id = newId(), collapsed = false) {
    return { id, name, info, collapsed, kids: [], parent: null };
}

export const hidesKids = n => n.collapsed || n.kids.length === 0;

export function depth(n) {
    let d = 0;
    for (let p = n.parent; p; p = p.parent) ++d;
    return d;
}

export function countDesc(n) {
    let c = 0;
    const stack = [...n.kids];
    while (stack.length) { const k = stack.pop(); ++c; stack.push(...k.kids); }
    return c;
}

export function contains(a, n) {  // n is a or one of its descendants
    for (; n; n = n.parent) if (n === a) return true;
    return false;
}

export function walk(root, f) {  // pre-order, iterative
    const stack = [root];
    while (stack.length) {
        const n = stack.pop();
        f(n);
        for (let i = n.kids.length - 1; i >= 0; --i) stack.push(n.kids[i]);
    }
}

function addChild(p, c) { c.parent = p; p.kids.push(c); return c; }

function detach(n) {
    const v = n.parent.kids;
    v.splice(v.indexOf(n), 1);
    n.parent = null;
    return n;
}

// Children keep file order. Repairs: duplicate/missing ids get fresh ones; nodes
// whose parent is missing, or that sit on a parent cycle, are dropped.
export function fromJson(top) {
    if (!top || typeof top !== 'object' || Array.isArray(top)) throw new Error('top level must be an object');
    if (!('nodes' in top) && ('children' in top || 'name' in top)) return fromNested(top);
    const recs = [], byId = new Map();
    for (const o of Array.isArray(top.nodes) ? top.nodes : []) {
        if (!o || typeof o !== 'object') continue;
        let id = typeof o.id === 'string' ? o.id : '';
        if (!id || byId.has(id)) id = newId();
        byId.set(id, recs.length);
        recs.push({ n: makeNode(typeof o.name === 'string' ? o.name : 'Unnamed', typeof o.info === 'string' ? o.info : '',
                                id, o.collapsed === true),
                    parent: typeof o.parent === 'string' ? o.parent : '' });
    }
    const rootIdx = byId.get(top.root);
    if (rootIdx === undefined) throw new Error('missing or unknown "root"');
    const state = new Uint8Array(recs.length);  // 0 unknown, 1 visiting, 2 ok, 3 bad
    state[rootIdx] = 2;
    for (let i = 0; i < recs.length; ++i) {
        const chain = [];
        let j = i;
        while (state[j] === 0) {
            state[j] = 1;
            chain.push(j);
            const p = byId.get(recs[j].parent);
            if (p === undefined) { state[j] = 3; break; }
            j = p;
        }
        const verdict = state[j] === 2 ? 2 : 3;
        for (const c of chain) state[c] = verdict;
    }
    for (let i = 0; i < recs.length; ++i)
        if (i !== rootIdx && state[i] === 2) addChild(recs[byId.get(recs[i].parent)].n, recs[i].n);
    return recs[rootIdx].n;
}

// Older nested format: {name, info, collapsed, id, children: [...]}.
function fromNested(top) {
    const seen = new Set();
    const mk = o => {
        let id = typeof o.id === 'string' ? o.id : '';
        if (!id || seen.has(id)) id = newId();
        seen.add(id);
        return makeNode(typeof o.name === 'string' ? o.name : 'Unnamed', typeof o.info === 'string' ? o.info : '', id, o.collapsed === true);
    };
    const root = mk(top), stack = [[top, root]];
    while (stack.length) {
        const [o, n] = stack.pop();
        for (const c of Array.isArray(o.children) ? o.children : [])
            if (c && typeof c === 'object') { const k = addChild(n, mk(c)); stack.push([c, k]); }
    }
    return root;
}

export function toJson(root) {
    const nodes = [];
    walk(root, n => {
        const o = { id: n.id };
        if (n.parent) o.parent = n.parent.id;
        Object.assign(o, { name: n.name, info: n.info, collapsed: n.collapsed });
        nodes.push(o);
    });
    return { root: root.id, nodes };
}

// Owns the tree. Every mutation goes through commit(), which records undo
// history (whole-tree snapshots, like the desktop app) and notifies listeners.
export class Doc {
    constructor() {
        this.root = null;
        this.name = '';
        this.index = new Map();
        this.undoStack = [];
        this.redoStack = [];
        this.current = null;
        this.onChange = () => {};
    }

    find(id) { return id ? this.index.get(id) || null : null; }

    load(root, name) {
        this.root = root;
        this.name = name;
        this.reindex();
        this.current = toJson(root);
        this.undoStack = [];
        this.redoStack = [];
        this.onChange('open');
    }

    reindex() {
        this.index.clear();
        if (this.root) walk(this.root, n => this.index.set(n.id, n));
    }

    commit() {
        this.undoStack.push(this.current);
        if (this.undoStack.length > 200) this.undoStack.shift();
        this.redoStack = [];
        this.current = toJson(this.root);
        this.reindex();
        this.onChange('edit');
    }

    step(from, to) {
        if (!from.length) return false;
        to.push(this.current);
        this.current = from.pop();
        this.root = fromJson(this.current);
        this.reindex();
        this.onChange('edit');
        return true;
    }
    undo() { return this.step(this.undoStack, this.redoStack); }
    redo() { return this.step(this.redoStack, this.undoStack); }

    addChild(n, name, info = '') {
        const c = addChild(n, makeNode(name, info));
        n.collapsed = false;
        this.commit();
        return c;
    }
    removeSubtree(n) {
        if (!n.parent) return 'Cannot delete the root clade';
        detach(n);
        this.commit();
        return '';
    }
    moveSubtree(n, target) {
        if (!n.parent) return 'Cannot move the root clade';
        if (contains(n, target)) return 'Cannot paste a clade into itself or its descendants';
        addChild(target, detach(n));
        target.collapsed = false;
        this.commit();
        return '';
    }
}
