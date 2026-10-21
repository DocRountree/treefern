// The tree canvas: camera, drawing, labels, hit testing and pointer input.
// Camera, culling and label placement follow the desktop app's Canvas.
import {
    PI, C, add, sub, mul, scale, abs, norm, arg, polar, remainder, clamp, Local, Frame,
    compose, uncompose, finiteFrame, lerpZ, lerpLocal, easeOutCubic, easeInOutCubic,
    mmul, mapply, minv, mrot, mtoZero, mOrigin, mDeriv0, mnormalize,
} from './geom.js';
import { Layout, DISKS, PYTHAGORAS, HYPERBOLIC, STD_NODE, STD_WIDTH, PY_W, HYP_EDGE, branchy } from './layout.js';
import { hidesKids, depth as nodeDepth, contains, countDesc } from './model.js';

const ANIM_MS = 380;            // relayout animation; camera flights take twice as long
const ZOOM_STEP = 1.15, FAST_ZOOM_STEP = 2.0;
const MAX_LABELS = 400;
const LABEL_MIN_SCREEN_LEN = 35;  // disks: smaller discs get no label
const LABEL_GAP = 2;
const WRAP_COST = 0.12;
const MAX_WRAP_LINES = 6;
const STEM_COST = 0.8;
const STD_CURVE = 0.1;
const MIN_PX = 0.6;
const FIT_MARGIN = 120;
const CAM_MAX_SCALE = 1e9, CAM_MIN_ROOT_PX = 20, CAM_MAX_UP = 1e9;
const LONG_PRESS_MS = 500, DOUBLE_TAP_MS = 320;
const MORPH_MS = 900;            // switching between layouts
const HYP_MIN_PX = 0.35;         // hyperbolic: cull subtrees whose nodes are smaller than this
const HYP_UNIT = Math.tanh(HYP_EDGE / 2);  // Euclidean length of an edge at the disk centre
export const FONT = 'Inter, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif';

export const THEMES = {
    dark: { dark: true, bg: [14, 17, 26], fg: [225, 230, 240], dim: [130, 140, 160], accent: [255, 200, 80],
            cut: [255, 90, 90], labelBg: 'rgba(20,24,36,0.745)', labelBgHover: 'rgba(28,34,50,0.94)', sat: 0.55, val: 0.95 },
    light: { dark: false, bg: [246, 247, 250], fg: [28, 32, 42], dim: [105, 113, 130], accent: [200, 120, 0],
             cut: [215, 45, 45], labelBg: 'rgba(255,255,255,0.8)', labelBgHover: 'rgba(255,255,255,0.96)', sat: 0.70, val: 0.72 },
};

const css = ([r, g, b]) => `rgb(${r},${g},${b})`;
function hsv(h, s, v) {
    const f = (n, k = (n + h / 60) % 6) => v - v * s * Math.max(Math.min(k, 4 - k, 1), 0);
    return [f(5) * 255, f(3) * 255, f(1) * 255].map(Math.round);
}
// Qt's QColor::darker(factor): divide the HSV value by factor/100.
function darker([r, g, b], factor) {
    const k = 100 / factor;
    return [r * k, g * k, b * k].map(Math.round);
}

// Liang–Barsky: clip segment a-b to rect r; null if fully outside.
function clipLine(r, a, b) {
    let t0 = 0, t1 = 1;
    const dx = b.x - a.x, dy = b.y - a.y;
    const p = [-dx, dx, -dy, dy], q = [a.x - r.x0, r.x1 - a.x, a.y - r.y0, r.y1 - a.y];
    for (let i = 0; i < 4; ++i) {
        if (p[i] === 0) { if (q[i] < 0) return false; continue; }
        const t = q[i] / p[i];
        if (p[i] < 0) { if (t > t1) return false; t0 = Math.max(t0, t); }
        else { if (t < t0) return false; t1 = Math.min(t1, t); }
    }
    return true;
}
function segDist(p, a, b) {
    const dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const t = l2 > 0 ? clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0, 1) : 0;
    return Math.hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}
const R = (x0, y0, x1, y1) => ({ x0, y0, x1, y1 });
const rectWH = (x, y, w, h) => R(x, y, x + w, y + h);
const inflate = (r, d) => R(r.x0 - d, r.y0 - d, r.x1 + d, r.y1 + d);
const hits = (a, b) => a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
const inside = (outer, r) => r.x0 >= outer.x0 && r.y0 >= outer.y0 && r.x1 <= outer.x1 && r.y1 <= outer.y1;
const has = (r, p) => p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1;

// Box of size w*h whose side facing direction (ux, uy) touches (px, py).
function anchoredRect(w, h, px, py, ux, uy) {
    const l = ux > 0.35 ? px : ux < -0.35 ? px - w : px - w / 2;
    const t = uy > 0.35 ? py : uy < -0.35 ? py - h : py - h / 2;
    return rectWH(Math.round(l), Math.round(t), w, h);
}

// Outline of a tapered quadratic branch a -> b (control c), widths wa -> wb.
function taper(a, c, b, wa, wb, N = 12) {
    const left = [], right = [];
    for (let i = 0; i <= N; ++i) {
        const t = i / N, u = 1 - t;
        const p = { x: a.x * u * u + c.x * 2 * u * t + b.x * t * t, y: a.y * u * u + c.y * 2 * u * t + b.y * t * t };
        let tx = (c.x - a.x) * 2 * u + (b.x - c.x) * 2 * t, ty = (c.y - a.y) * 2 * u + (b.y - c.y) * 2 * t;
        let len = Math.hypot(tx, ty);
        if (len < 1e-9) { tx = b.x - a.x; ty = b.y - a.y; len = Math.max(1e-9, Math.hypot(tx, ty)); }
        const w = (wa + (wb - wa) * t) / 2;
        left.push({ x: p.x - ty / len * w, y: p.y + tx / len * w });
        right.push({ x: p.x + ty / len * w, y: p.y - tx / len * w });
    }
    return left.concat(right.reverse());
}

const mixPt = (a, b, t) => C(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);

// Andrew's monotone chain.
function convexHull(pts) {
    const p = [...pts].sort((a, b) => a.x - b.x || a.y - b.y);
    const cross = (o, a, b) => (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    const lo = [], hi = [];
    for (const q of p) { while (lo.length >= 2 && cross(lo.at(-2), lo.at(-1), q) <= 0) lo.pop(); lo.push(q); }
    for (const q of p.reverse()) { while (hi.length >= 2 && cross(hi.at(-2), hi.at(-1), q) <= 0) hi.pop(); hi.push(q); }
    return lo.slice(0, -1).concat(hi.slice(0, -1));
}

export class TreeView {
    constructor(canvas, doc, handlers) {
        this.canvas = canvas;
        this.ctx = canvas.getContext('2d');
        this.doc = doc;
        this.h = handlers;  // {select(id), edit(id), menu(id, clientX, clientY), viewChanged()}
        this.lay = new Layout();
        this.theme = THEMES.dark;
        this.labelsBeside = false;
        this.selId = '';
        this.cutId = '';
        this.hoverId = '';
        this.showHint = true;
        this.cam = { anchor: '', f: Frame(C(0, 0), C(300, 0)) };
        // hyperbolic camera: the anchor node's frame -> the unit disk (a Möbius map),
        // and the disk's radius and centre on screen
        this.hcam = { anchor: '', A: mrot(-PI / 2), R: 100 };  // the disk stays centred: see hD()
        this.hAnim = null;     // hyperbolic flight {start, A0, q, turn}
        this.morph = null;     // layout morph {start, from, to}
        this.shownLoc = new Map();
        this.shownLocById = new Map();
        this.fromLoc = null;
        this.anim = null;      // relayout animation {start}
        this.camAnim = null;   // camera flight {start, from, to}
        this.following = false;
        this.followPrev = null;
        this.items = [];
        this.branches = [];
        this.hitSegs = [];
        this.labelHits = [];
        this.width = 300;
        this.height = 200;
        this.insetBottom = 0;
        this.textW = new Map();
        this.pointers = new Map();
        this.lastTap = { t: 0, x: 0, y: 0 };
        this.frameReq = 0;
        this.bindInput();
        new ResizeObserver(() => this.resize()).observe(canvas);
        this.resize();
    }

    // ---- public API
    disks() { return this.lay.style === DISKS; }
    hyp() { return this.lay.style === HYPERBOLIC; }
    py() { return this.lay.style === PYTHAGORAS; }

    // Switch layout, morphing every visible node from where it is now.
    setStyle(st) {
        if (st === this.lay.style) return;
        const from = this.doc.root && this.items.length ? this.snapshot() : null;
        this.lay.style = st;
        this.lay.compute(this.doc.root);
        this.anim = this.camAnim = this.hAnim = null;
        this.following = false;
        this.fromLoc = null;
        this.shownLoc = new Map();
        this.shownLocById = new Map();
        this.resetCam();
        const n = this.doc.find(this.selId);  // keep the selection in view
        if (n && this.lay.loc.has(n)) {
            const f = this.screenFrame(n);
            if (!f || !has(R(40, 40, this.width - 40, this.height - this.insetBottom - 40), f.o) || abs(f.z) < 12) this.focusNow(n);
        }
        if (from) this.startMorph(from);
        else this.viewUpdated();
    }

    resize() {
        const r = this.canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
        const w = Math.max(1, r.width), h = Math.max(1, r.height);
        const first = this.width === 300 && this.height === 200;
        const oc = this.centre();
        this.width = w; this.height = h;
        this.canvas.width = Math.round(w * dpr);
        this.canvas.height = Math.round(h * dpr);
        this.dpr = dpr;
        if (!first) {  // keep the view centred
            this.cam.f.o = add(this.cam.f.o, sub(this.centre(), oc));
        }
        this.draw();
    }

    // Recompute the layout and animate from what is on screen now.
    relayout(animate = true) {
        if (this.hyp()) {  // edits morph in screen space
            const from = animate && this.items.length ? this.snapshot() : null;
            this.lay.compute(this.doc.root);
            this.anim = this.camAnim = null;
            this.shownLoc = new Map();
            this.shownLocById = new Map();
            this.hFixAnchor();
            if (from) this.startMorph(from, ANIM_MS * 1.4);
            else this.draw();
            return;
        }
        this.fromLoc = animate && this.shownLocById.size ? this.shownLocById : null;
        this.lay.compute(this.doc.root);
        this.camAnim = null;
        this.anim = null;
        this.havePrev = false;
        this.shownLoc = new Map();
        this.fixAnchor();
        this.following = !!this.fromLoc && !!this.selId;
        this.applyFrame(this.fromLoc ? 0 : 1);
        if (this.fromLoc) { this.anim = { start: performance.now() }; this.loop(); }
    }

    fitView() {
        this.following = false;
        this.resetCam();
        this.viewUpdated();
    }

    focusOn(n) {
        if (!n || !this.lay.loc.has(n)) return;
        if (this.hyp()) return this.hFocus(n);
        let mag = Math.min(this.width, this.height) * 0.42;
        if (!this.disks()) {
            const f = this.screenFrame(n);
            mag = Math.min(Math.max(f ? abs(f.z) : 120, 120), mag / this.lay.extent(n));
        }
        this.flyTo(n, Frame(this.centre(), polar(mag, this.absAngle(n))));
    }

    ensureVisible(n) {
        if (!n || !this.lay.loc.has(n)) return;
        const f = this.screenFrame(n);
        if (!f) return this.focusOn(n);
        const s = abs(f.z), lo = Math.min(this.width, this.height);
        const ok = has(R(60, 60, this.width - 60, this.height - 60), f.o);
        if (!ok || (this.disks() ? s < 30 || s > lo * 0.6 : s < 15)) this.focusOn(n);
    }

    zoomAt(factor, p = this.centre()) {
        this.following = false;
        this.zoomBy(factor, p);
        this.viewUpdated();
    }

    // Zoom relative to the fitted view, accumulated in log space to survive extreme depths.
    zoomText() {
        if (this.hyp()) return (this.hcam.R / this.hDefaultR()).toFixed(2) + 'x';
        const a = this.anchorNode();
        if (!a) return '-';
        let z = Math.log10(abs(this.cam.f.z) / this.fitScale());
        for (let x = a; x; x = x.parent) z -= Math.log10(abs(this.sloc(x).z));
        return z < 4 ? Math.pow(10, z).toFixed(2) + 'x' : `10^${z.toFixed(1)}x`;
    }

    // Render the current view into a PNG blob at `factor` times its CSS size.
    exportPng(factor = 2) {
        const c = document.createElement('canvas');
        c.width = Math.round(this.width * factor);
        c.height = Math.round(this.height * factor);
        const saved = [this.ctx, this.dpr, this.showHint, this.hoverId];
        this.ctx = c.getContext('2d');
        this.dpr = factor;
        this.showHint = false;
        this.hoverId = '';
        this.render();
        [this.ctx, this.dpr, this.showHint, this.hoverId] = saved;
        this.draw();
        return new Promise(res => c.toBlob(res, 'image/png'));
    }

    draw() {
        if (this.frameReq) return;
        this.frameReq = requestAnimationFrame(() => { this.frameReq = 0; this.render(); });
    }

    // ---- camera
    // Centre of the part of the canvas not covered by the phone bottom sheet.
    centre() { return C(this.width / 2, (this.height - this.insetBottom) / 2); }
    anchorNode() { return this.doc.find(this.cam.anchor); }
    inCut(n) { const c = this.doc.find(this.cutId); return !!c && contains(c, n); }
    sloc(n) { return this.shownLoc.get(n) || this.lay.loc.get(n) || Local(); }
    viewUpdated() { this.draw(); this.h.viewChanged(); }

    absAngle(n) {
        let a = 0;
        for (let x = n; x; x = x.parent) a += arg((this.lay.loc.get(x) || Local()).z);
        return a;
    }

    covers(c, r) {  // the circle contains the whole view
        for (const q of [C(0, 0), C(this.width, 0), C(0, this.height), C(this.width, this.height)])
            if (norm(sub(q, c)) > r * r) return false;
        return true;
    }

    fitScale() {
        const b = this.lay.bbox;
        const bw = Math.max(b.x1 - b.x0, 2), bh = Math.max(b.y1 - b.y0, 2);
        const margin = Math.min(FIT_MARGIN, Math.min(this.width, this.height) * 0.15);
        return Math.min(Math.max(100, this.width - margin) / bw, Math.max(100, this.height - this.insetBottom - margin) / bh);
    }

    resetCam() {
        if (this.hyp()) return this.hReset();
        this.camAnim = null;
        const r = this.doc.root;
        if (!r || !this.lay.loc.has(r)) return;
        const s = this.fitScale(), b = this.lay.bbox;
        const bc = C((b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2);
        this.cam.anchor = r.id;
        this.cam.f = compose(Frame(sub(this.centre(), scale(bc, s)), C(s, 0)), this.lay.loc.get(r));
        this.reanchor();
    }

    fixAnchor() {
        if (!this.doc.root) return;
        let a = this.anchorNode();
        if (!a) return this.resetCam();
        while (!this.lay.loc.has(a) && a.parent) {
            this.cam.f = uncompose(this.cam.f, (this.fromLoc && this.fromLoc.get(a.id)) || Local());
            a = a.parent;
        }
        this.cam.anchor = a.id;
    }

    screenFrame(n) {
        if (this.hyp()) { const m = this.hMatrix(n); return m ? this.hFrame(m) : null; }
        const a = this.anchorNode();
        if (!a || !n || !this.lay.loc.has(n)) return null;
        let l = a;
        while (!contains(l, n)) l = l.parent;
        let d = this.cam.f;
        for (let x = a; x !== l; x = x.parent) d = uncompose(d, this.sloc(x));
        const path = [];
        for (let x = n; x !== l; x = x.parent) path.push(x);
        for (let i = path.length - 1; i >= 0; --i) d = compose(d, this.sloc(path[i]));
        return finiteFrame(d) ? d : null;
    }

    stemFrom(n, f) {
        if (n.parent) return uncompose(f, this.sloc(n));
        if (branchy(this.lay.style)) return Frame(sub(f.o, f.z), f.z);
        return null;
    }

    // Move the anchor up while its subtree is small on screen, and down into the
    // child under the view centre while that child is big.
    reanchor() {
        if (this.hyp()) return this.hReanchor();
        if (this.camAnim) return;
        let a = this.anchorNode();
        if (!a) return;
        const lo = Math.max(1, Math.min(this.width, this.height)), hi = Math.max(this.width, this.height);
        for (let guard = 0; guard < 100000; ++guard) {
            const Rr = abs(this.cam.f.z) * this.lay.extent(a);
            if (a.parent && Rr < lo * 0.25) {
                this.cam.f = uncompose(this.cam.f, this.sloc(a));
                a = a.parent;
                continue;
            }
            let next = null;
            if (Rr > hi * 2 && !hidesKids(a))
                for (const k of a.kids) {
                    const cf = compose(this.cam.f, this.sloc(k));
                    const kr = abs(cf.z) * this.lay.extent(k);
                    if (kr > lo * 0.5 && abs(sub(cf.o, this.centre())) < kr) { next = k; this.cam.f = cf; break; }
                }
            if (!next) break;
            a = next;
        }
        this.cam.anchor = a.id;
    }

    flyTo(n, target) {
        this.camAnim = null;
        this.following = false;
        const d = this.screenFrame(n);
        this.cam.anchor = n.id;
        if (d && abs(d.z) >= 1e-6 && abs(d.z) <= 1e12) {
            this.cam.f = d;
            this.camAnim = { start: performance.now(), from: d, to: target };
            this.loop();
            return;
        }
        this.cam.f = target;  // too far off to animate sensibly: jump
        this.reanchor();
        this.viewUpdated();
    }

    // Pan by d; `at` is where the pointer is now (hyperbolic pans need both ends).
    panBy(d, at = null) {
        if (this.hyp()) {
            const to = at || add(this.centre(), d);
            this.hAnim = null;
            return this.hPan(sub(to, d), to);
        }
        this.camAnim = null;
        this.cam.f = Frame(add(this.cam.f.o, d), this.cam.f.z);
        this.reanchor();
    }

    zoomBy(f, p) {
        if (this.hyp()) {
            // the disk stays centred and grows about its centre; drag slides the tree in it
            const d = this.hDefaultR();
            this.hcam.R = clamp(this.hcam.R * f, d * 0.6, d * 4);
            return;
        }
        this.camAnim = null;
        const s = abs(this.cam.f.z);
        if (s * f > CAM_MAX_SCALE) f = CAM_MAX_SCALE / s;
        const r = this.doc.root, rf = r && this.screenFrame(r);
        if (rf) {
            const rr = abs(rf.z) * this.lay.extent(r);
            if (rr * f < CAM_MIN_ROOT_PX) f = CAM_MIN_ROOT_PX / rr;
        }
        this.cam.f = Frame(add(p, scale(sub(this.cam.f.o, p), f)), scale(this.cam.f.z, f));
        this.reanchor();
    }

    // Interpolate parent-relative frames; new nodes grow out of their parent's origin.
    applyFrame(t) {
        this.shownLoc = new Map();
        const byId = new Map();
        for (const [n, L0] of this.lay.loc) {
            let L = L0;
            if (t < 1 && n.parent && this.fromLoc)
                L = lerpLocal(this.fromLoc.get(n.id) || Local(C(0, 0), scale(L0.z, 0.02), L0.dir), L0, t);
            this.shownLoc.set(n, L);
            byId.set(n.id, L);
        }
        this.shownLocById = byId;
        this.levelCamera();
        if (this.following && !this.camAnim) {
            const d = this.screenFrame(this.doc.find(this.selId));
            if (d) {
                if (this.havePrev) this.cam.f = Frame(add(this.cam.f.o, sub(this.followPrev, d.o)), this.cam.f.z);
                else { this.followPrev = d.o; this.havePrev = true; }
            }
        }
        this.draw();
    }

    // The view never rotates: counter-rotate the camera after a relayout.
    levelCamera() {
        if (this.camAnim || this.hyp()) return;
        const a = this.anchorNode();
        if (!a) return;
        let want = 0;
        for (let x = a; x; x = x.parent) want += arg(this.sloc(x).z);
        const err = remainder(want - arg(this.cam.f.z), 2 * PI);
        if (Math.abs(err) < 1e-12) return;
        let pivot = this.centre();
        const s = this.screenFrame(this.doc.find(this.selId));
        if (s && has(R(0, 0, this.width, this.height), s.o)) pivot = s.o;
        const rot = polar(1, err);
        this.cam.f = Frame(add(pivot, mul(rot, sub(this.cam.f.o, pivot))), mul(rot, this.cam.f.z));
    }

    loop() {
        if (this.looping) return;
        this.looping = true;
        const step = now => {
            if (this.anim) {
                const t = Math.min(1, (now - this.anim.start) / ANIM_MS);
                this.applyFrame(easeOutCubic(t));
                if (t >= 1) { this.anim = null; this.following = false; this.reanchor(); }
            }
            if (this.camAnim) {
                const { start, from, to } = this.camAnim;
                const t = Math.min(1, (now - start) / (2 * ANIM_MS)), e = easeInOutCubic(t);
                this.cam.f = Frame(add(from.o, scale(sub(to.o, from.o), e)), lerpZ(from.z, to.z, e, true));
                if (t >= 1) { this.camAnim = null; this.reanchor(); }
                this.viewUpdated();
            }
            if (this.hAnim) {
                const { start, A0, q, turn } = this.hAnim;
                const t = Math.min(1, (now - start) / (2 * ANIM_MS)), e = easeInOutCubic(t);
                const r = abs(q);
                const c = r < 1e-12 ? C(0) : scale(q, Math.tanh(e * Math.atanh(Math.min(r, 1 - 1e-15))) / r);
                this.hcam.A = mnormalize(mmul(mrot(turn * e), mmul(mtoZero(c), A0)));
                if (t >= 1) { this.hAnim = null; this.hReanchor(); }
                this.viewUpdated();
            }
            if (this.morph) {
                if (now - this.morph.start >= this.morph.ms) this.morph = null;
                this.viewUpdated();
            }
            if (this.anim || this.camAnim || this.hAnim || this.morph) requestAnimationFrame(step);
            else this.looping = false;
        };
        requestAnimationFrame(step);
    }

    // ---- visible items
    makeItem(n, f, d) {
        const s = abs(f.z), leaf = hidesKids(n);
        const r = this.disks() ? s : this.py() ? Math.max(3, s * PY_W)
                : leaf ? Math.max(3, s * STD_NODE) : Math.max(3.5, s * STD_WIDTH * 0.65);
        let len = s;
        if (branchy(this.lay.style) && leaf && n.parent) {
            const L = this.sloc(n);
            if (abs(L.z) > 0) len = s / abs(L.z) * abs(L.o);
        }
        return { n, f, s: f.o, r, lenPx: s, angle: arg(f.z) + this.sloc(n).dir, depth: d, branchPx: len };
    }

    computeItems() {
        this.items = [];
        this.branches = [];
        if (this.hyp()) return this.hComputeItems();
        const A = this.anchorNode();
        if (!A || !this.lay.loc.has(A)) return;
        const margin = this.disks() ? 0 : 100, minPx = this.disks() ? MIN_PX : 0;
        const visible = (n, f) => {
            const Rr = abs(f.z) * this.lay.extent(n);
            return Rr >= minPx && f.o.x + Rr > -margin && f.o.x - Rr < this.width + margin &&
                   f.o.y + Rr > -margin && f.o.y - Rr < this.height + margin;
        };
        const down = (n, f, d, skip) => {
            if (!visible(n, f)) return;
            this.items.push(this.makeItem(n, f, d));
            if (hidesKids(n)) return;
            for (const k of n.kids)
                if (k !== skip) {
                    const cf = compose(f, this.sloc(k));
                    this.branches.push({ child: k, pf: f, cf, depth: d });
                    down(k, cf, d + 1, null);
                }
        };
        let d = nodeDepth(A);
        const pf = this.stemFrom(A, this.cam.f);
        if (pf) this.branches.push({ child: A, pf, cf: this.cam.f, depth: d - 1 });
        down(A, this.cam.f, d, null);
        let fr = this.cam.f;
        for (let c = A; c.parent; c = c.parent, --d) {
            if (this.disks() && this.covers(fr.o, abs(fr.z))) break;
            const pd = uncompose(fr, this.sloc(c));
            if (!finiteFrame(pd) || abs(pd.z) > CAM_MAX_UP) break;
            const ppf = this.stemFrom(c.parent, pd);
            if (ppf) this.branches.push({ child: c.parent, pf: ppf, cf: pd, depth: d - 2 });
            down(c.parent, pd, d - 1, c);
            fr = pd;
        }
        this.items.sort((a, b) => a.depth - b.depth);
    }

    depthColor(d, bright = 1) {
        const t = this.theme;
        return hsv(((200 + d * 37) % 360 + 360) % 360, t.sat, Math.min(1, t.val * bright));
    }

    // ---- painting
    render() {
        const ctx = this.ctx, t = this.theme;
        ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
        ctx.fillStyle = css(t.bg);
        ctx.fillRect(0, 0, this.width, this.height);
        this.labelHits = [];
        this.hitSegs = [];
        if (!this.doc.root) { this.items = []; this.branches = []; return; }
        if (this.morph) return this.renderMorph();
        this.computeItems();
        if (this.hyp()) this.drawDisk(1);
        const segs = this.py() ? this.drawBlocks() : this.drawBranches();
        this.drawNodes();
        this.drawLabels(segs);
    }

    drawDisk(alpha) {
        const ctx = this.ctx, r = this.hcam.R, D = this.hD(), dark = this.theme.dark;
        ctx.globalAlpha = alpha;
        ctx.beginPath();
        ctx.arc(D.x, D.y, r, 0, 2 * PI);
        ctx.fillStyle = dark ? 'rgba(255,255,255,0.025)' : 'rgba(0,0,0,0.025)';
        ctx.fill();
        ctx.lineWidth = 1;
        ctx.strokeStyle = dark ? 'rgba(130,140,160,0.35)' : 'rgba(105,113,130,0.35)';
        ctx.stroke();
        ctx.globalAlpha = 1;
    }

    drawBranches() {
        const segs = [];
        if (this.disks()) return segs;
        const ctx = this.ctx, view = R(-60, -60, this.width + 60, this.height + 60);
        for (const br of this.branches) {
            const a = br.pf.o, b = br.cf.o, ps = abs(br.pf.z);
            const wa = Math.max(1, ps * STD_WIDTH), wb = Math.max(1, abs(br.cf.z) * STD_WIDTH), m = Math.max(wa, wb);
            const ctrl = br.ctrl || add(a, scale(br.pf.z, Math.hypot(b.x - a.x, b.y - a.y) * STD_CURVE / ps));
            const bb = R(Math.min(a.x, b.x, ctrl.x) - m, Math.min(a.y, b.y, ctrl.y) - m,
                         Math.max(a.x, b.x, ctrl.x) + m, Math.max(a.y, b.y, ctrl.y) + m);
            if (!hits(view, bb)) continue;
            const col = this.inCut(br.child) ? darker(this.theme.cut, 140) : this.depthColor(br.depth, 0.7);
            const N = 12, mid = [], left = [], right = [];
            for (let i = 0; i <= N; ++i) {
                const tt = i / N, u = 1 - tt;
                const p = { x: a.x * u * u + ctrl.x * 2 * u * tt + b.x * tt * tt, y: a.y * u * u + ctrl.y * 2 * u * tt + b.y * tt * tt };
                mid.push(p);
                let tx = (ctrl.x - a.x) * 2 * u + (b.x - ctrl.x) * 2 * tt, ty = (ctrl.y - a.y) * 2 * u + (b.y - ctrl.y) * 2 * tt;
                let len = Math.hypot(tx, ty);
                if (len < 1e-9) { tx = b.x - a.x; ty = b.y - a.y; len = Math.max(1e-9, Math.hypot(tx, ty)); }
                const w = (wa + (wb - wa) * tt) / 2;
                left.push({ x: p.x - ty / len * w, y: p.y + tx / len * w });
                right.push({ x: p.x + ty / len * w, y: p.y - tx / len * w });
            }
            ctx.beginPath();
            for (const [i, p] of left.entries()) i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y);
            for (let i = right.length - 1; i >= 0; --i) ctx.lineTo(right[i].x, right[i].y);
            ctx.closePath();
            ctx.fillStyle = css(col);
            ctx.fill();
            for (let i = 0; i + 1 < mid.length; ++i) {
                const w = Math.max(wa + (wb - wa) * (i / N), wa + (wb - wa) * ((i + 1) / N));
                this.hitSegs.push({ a: mid[i], b: mid[i + 1], hw: w / 2, n: br.child });
                const pad = w / 2 + 1;
                segs.push({ a: mid[i], b: mid[i + 1], pad,
                            box: inflate(R(Math.min(mid[i].x, mid[i + 1].x), Math.min(mid[i].y, mid[i + 1].y),
                                           Math.max(mid[i].x, mid[i + 1].x), Math.max(mid[i].y, mid[i + 1].y)), pad),
                            p: br.child.parent, c: br.child });
            }
        }
        return segs;
    }

    drawNodes() {
        const ctx = this.ctx, dk = this.disks(), t = this.theme;
        for (const v of this.py() ? [] : this.items) {
            const cut = this.inCut(v.n);
            const fill = cut ? darker(t.cut, 160) : this.depthColor(v.depth, dk ? 0.55 : 1);
            if (this.covers(v.s, v.r)) { ctx.fillStyle = css(fill); ctx.fillRect(0, 0, this.width, this.height); continue; }
            ctx.beginPath();
            ctx.arc(v.s.x, v.s.y, v.r, 0, 2 * PI);
            ctx.fillStyle = css(fill);
            ctx.fill();
            ctx.lineWidth = clamp(v.r * (dk ? 0.02 : 0.12), 1, 3);
            ctx.strokeStyle = css(cut ? t.cut : this.depthColor(v.depth, dk ? 1 : 0.65));
            ctx.stroke();
        }
        const sel = this.doc.find(this.selId), cut = this.doc.find(this.cutId);
        ctx.lineWidth = 2;
        for (const v of this.items) {
            if (v.r > 1e5) continue;
            if (v.n === sel) {
                ctx.setLineDash([]);
                ctx.strokeStyle = css(t.accent);
                ctx.beginPath(); ctx.arc(v.s.x, v.s.y, v.r + 4, 0, 2 * PI); ctx.stroke();
            }
            if (v.n === cut) {
                ctx.setLineDash([6, 4]);
                ctx.strokeStyle = css(t.cut);
                ctx.beginPath(); ctx.arc(v.s.x, v.s.y, v.r + 8, 0, 2 * PI); ctx.stroke();
                ctx.setLineDash([]);
            }
        }
    }

    measure(font, s) {
        const key = font + '\u0000' + s;
        let w = this.textW.get(key);
        if (w === undefined) {
            this.ctx.font = font;
            w = this.ctx.measureText(s).width;
            if (this.textW.size > 20000) this.textW.clear();
            this.textW.set(key, w);
        }
        return w;
    }

    // For each line count k, the split that minimises the widest line; an option
    // is kept only if narrower than every option with fewer lines.
    wrapOptions(words, font) {
        const n = words.length, pre = [0];
        for (let i = 0; i < n; ++i) pre.push(pre[i] + this.measure(font, words[i]));
        const sp = this.measure(font, ' ');
        const W = (a, b) => pre[b] - pre[a] + (b - a - 1) * sp;
        const K = Math.min(n, MAX_WRAP_LINES);
        const best = Array.from({ length: K + 1 }, () => new Array(n + 1).fill(Infinity));
        const cut = Array.from({ length: K + 1 }, () => new Array(n + 1).fill(0));
        best[0][0] = 0;
        for (let k = 1; k <= K; ++k)
            for (let j = k; j <= n; ++j)
                for (let i = k - 1; i < j; ++i) {
                    const m = Math.max(best[k - 1][i], W(i, j));
                    if (m < best[k][j]) { best[k][j] = m; cut[k][j] = i; }
                }
        const out = [];
        let prevW = Infinity;
        for (let k = 1; k <= K; ++k) {
            if (!(best[k][n] < prevW - 0.5)) continue;
            const lines = [];
            for (let kk = k, j = n; kk > 0; j = cut[kk][j], --kk) lines.unshift(words.slice(cut[kk][j], j).join(' '));
            out.push({ lines, width: best[k][n] });
            prevW = best[k][n];
        }
        return out;
    }

    // Greedy placement, selection first, then well-sized clades (see the desktop app).
    drawLabels(segs, dk = this.disks()) {
        const ctx = this.ctx, t = this.theme, beside = this.labelsBeside;
        const sel = this.doc.find(this.selId), cutN = this.doc.find(this.cutId), hov = this.doc.find(this.hoverId);
        const padX = 5, padY = 2;
        const OFFS = [0, .45, -.45, .9, -.9, 1.4, -1.4, 2.0, -2.0, 2.5, -2.5, PI];
        const view = R(0, 0, this.width, this.height), inner = R(4, 4, this.width - 4, this.height - 4);
        const nearView = inflate(view, 120);
        const items = this.items;

        const CELL = 64, grid = new Map();
        const forCells = (r, f) => {
            if (!hits(r, nearView)) return;
            const c = R(Math.max(r.x0, nearView.x0), Math.max(r.y0, nearView.y0), Math.min(r.x1, nearView.x1), Math.min(r.y1, nearView.y1));
            for (let x = Math.floor(c.x0 / CELL); x <= Math.floor(c.x1 / CELL); ++x)
                for (let y = Math.floor(c.y0 / CELL); y <= Math.floor(c.y1 / CELL); ++y) f(x * 100003 + y);
        };
        segs.forEach((s, i) => forCells(s.box, k => { let v = grid.get(k); if (!v) grid.set(k, v = []); v.push(i); }));
        const stamp = new Int32Array(segs.length).fill(-1);
        let query = 0;
        const anySeg = (r, want) => {
            ++query;
            let hit = false;
            forCells(r, k => {
                if (hit) return;
                const v = grid.get(k);
                if (!v) return;
                for (const i of v) {
                    if (stamp[i] === query) continue;
                    stamp[i] = query;
                    const s = segs[i];
                    if (!hits(s.box, r) || !want(s)) continue;
                    if (clipLine(inflate(r, s.pad), s.a, s.b)) { hit = true; return; }
                }
            });
            return hit;
        };

        const nodeRects = dk ? [] : items.map(v => R(v.s.x - v.r - 2, v.s.y - v.r - 2, v.s.x + v.r + 2, v.s.y + v.r + 2));
        const placed = [];
        const blocked = (r, own) => {
            const me = items[own].n;
            if (me !== sel && !inside(inner, r)) return true;
            const tr = inflate(r, LABEL_GAP);
            if (placed.some(q => hits(q, tr))) return true;
            for (let i = 0; i < nodeRects.length; ++i) if (i !== own && hits(nodeRects[i], r)) return true;
            return anySeg(r, s => s.p !== me && s.c !== me);
        };
        const onOwnStem = (r, me) => anySeg(r, s => s.p === me || s.c === me);

        const makeLabel = (v, own, force) => {
            let words = v.n.name.split(/\s+/).filter(Boolean);
            if (!words.length) words = ['?'];
            if (v.n === cutN) words[0] = '✂ ' + words[0];
            if (v.n.collapsed && v.n.kids.length) words.push(`[+${countDesc(v.n)}]`);
            const px = clamp(Math.trunc(dk ? v.lenPx * 0.11 : v.lenPx * 0.14), 11, 28);
            const font = `${px}px ${FONT}`;
            const lineH = Math.round(px * 1.25);

            let c = v.s, rr = v.r;
            if (!beside) {
                if (dk && !hidesKids(v.n)) {
                    c = C(v.s.x, v.s.y - v.r * 0.82);
                    rr = Math.min(v.r * 0.1, 12) + 2;
                    if (v.r > 200 && c.y < 24 && v.s.y + v.r > 80)
                        c = C(clamp(c.x, 80, this.width - 80), clamp(24, v.s.y - v.r + 24, v.s.y));
                }
                if (!force && !has(nearView, c)) return null;
            }
            const opts = this.wrapOptions(words, font);
            const cs = [];
            opts.forEach((o, k) => {
                const w = Math.ceil(o.width) + 2 * padX, h = lineH * o.lines.length + 2 * padY;
                if (beside) {
                    const maxRings = (v.lenPx > 80 || force) ? 3 : 1;
                    for (let ring = 0; ring < maxRings; ++ring) {
                        const gap = v.r + 6 + ring * 22;
                        for (const off of OFFS) {
                            const ux = Math.cos(v.angle + off), uy = Math.sin(v.angle + off);
                            cs.push({ cost: ring + Math.abs(off) * 0.25 + WRAP_COST * k,
                                      r: anchoredRect(w, h, v.s.x + ux * gap, v.s.y + uy * gap, ux, uy), k, leader: ring > 0 });
                        }
                    }
                    return;
                }
                if (dk && !force && v.lenPx < 140 && w > v.r * 1.85) return;
                const mx = w / 2 + rr - 1, my = h / 2 + rr - 1;
                const steps = (v.lenPx > 90 || force) ? 3 : 2;
                for (let i = -steps; i <= steps; ++i)
                    for (let j = -steps; j <= steps; ++j) {
                        const dx = mx * i / 2, dy = my * j / 2;
                        const r = rectWH(Math.round(c.x + dx - w / 2), Math.round(c.y + dy - h / 2), w, h);
                        const q = C(clamp(c.x, r.x0, r.x1), clamp(c.y, r.y0, r.y1));
                        const dist = Math.hypot(q.x - c.x, q.y - c.y);
                        if (dist <= rr || (v.lenPx > 90 && dist <= rr + 36))
                            cs.push({ cost: Math.abs(dx) / w + Math.abs(dy) / h + WRAP_COST * k + (dist > rr ? 1.5 : 0),
                                      r, k, leader: dist > rr + 4 });
                    }
            });
            if (!cs.length) return null;
            if (segs.length) for (const cd of cs) if (onOwnStem(cd.r, v.n)) cd.cost += STEM_COST;
            cs.sort((a, b) => a.cost - b.cost);
            for (const cd of cs)
                if (!blocked(cd.r, own)) return { box: cd.r, lines: opts[cd.k].lines, font, lineH, n: v.n, target: c, forced: false, leader: cd.leader };
            if (!force) return null;
            return { box: cs[0].r, lines: opts[cs[0].k].lines, font, lineH, n: v.n, target: c, forced: true, leader: false };
        };

        const minLen = dk ? LABEL_MIN_SCREEN_LEN : 24;
        const cands = [];
        items.forEach((v, i) => { if (v.n === sel || v.branchPx >= minLen) cands.push(i); });
        const big = Math.max(this.width, this.height) * 1.2;
        const prio = v => [v.n !== sel ? 1 : 0, v.lenPx > big ? 1 : 0, -this.lay.weight(v.n), v.depth, -v.lenPx];
        const cmp = (a, b) => { for (let i = 0; i < a.length; ++i) if (a[i] !== b[i]) return a[i] - b[i]; return 0; };
        const keyed = cands.map(i => [prio(items[i]), i]).sort((a, b) => cmp(a[0], b[0]));

        const labels = [];
        let hovShown = false;
        for (const [, i] of keyed) {
            if (labels.length >= MAX_LABELS) break;
            const l = makeLabel(items[i], i, items[i].n === sel);
            if (l) { placed.push(l.box); labels.push(l); hovShown ||= l.n === hov; }
        }
        if (hov && !hovShown) {
            const i = items.findIndex(v => v.n === hov);
            if (i >= 0) { const l = makeLabel(items[i], i, true); if (l) labels.push(l); }
        }

        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        for (const l of labels) {
            const b = l.box, cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
            if (l.leader) {
                ctx.setLineDash([1.5, 3]);
                ctx.lineWidth = 1.2;
                ctx.strokeStyle = css(t.dim);
                ctx.beginPath(); ctx.moveTo(l.target.x, l.target.y); ctx.lineTo(cx, cy); ctx.stroke();
                ctx.setLineDash([]);
            }
            const isSel = l.n === sel, isHov = l.n === hov;
            ctx.beginPath();
            ctx.roundRect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, 5);
            ctx.fillStyle = isHov || l.forced ? t.labelBgHover : t.labelBg;
            ctx.fill();
            if (isSel || isHov) { ctx.lineWidth = 1; ctx.strokeStyle = css(isSel ? t.accent : t.dim); ctx.stroke(); }
            ctx.font = l.font;
            ctx.fillStyle = css(isSel ? t.accent : this.inCut(l.n) ? t.cut : t.fg);
            const top = cy - (l.lines.length - 1) * l.lineH / 2;
            l.lines.forEach((s, i) => ctx.fillText(s, cx, top + i * l.lineH + 0.5));
            this.labelHits.push([b, l.n.id]);
        }
    }

    // ---- pythagoras: every clade is a square ending at its fork; the convex
    // hull of a square's top edge and its children's bottom edges joins them
    // (the "triangle" of a Pythagoras tree, for any number of children). All of
    // it stays inside the circles the layout reserves, so subtrees never meet.
    drawBlocks() {
        const ctx = this.ctx, segs = [], view = R(-60, -60, this.width + 60, this.height + 60);
        const fill = (pts, col) => {
            ctx.beginPath();
            pts.forEach((p, i) => i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y));
            ctx.closePath();
            ctx.fillStyle = css(col);
            ctx.fill();
        };
        const colour = (n, d, b) => this.inCut(n) ? darker(this.theme.cut, 140) : this.depthColor(d, b);
        // square of item v: top edge centred on its fork, facing its growth direction
        const sq = new Map();
        for (const v of this.items) {
            const half = Math.max(0.6, PY_W * abs(v.f.z)), u = scale(v.f.z, 1 / abs(v.f.z));
            const nrm = scale(C(-u.y, u.x), half), base = sub(v.s, scale(u, 2 * half));
            sq.set(v.n, { v, half, top: [add(v.s, nrm), sub(v.s, nrm)], bottom: [sub(base, nrm), add(base, nrm)], base });
        }
        for (const v of this.items) {  // joints first, so squares sit on top of them
            const me = sq.get(v.n);
            if (hidesKids(v.n)) continue;
            // children in angular order share the top edge in proportion to their size
            const kids = this.lay.ordered(v.n).map(k => sq.get(k)).filter(Boolean);
            const total = kids.reduce((t, c) => t + c.half, 0);
            let acc = 0;
            for (const c of kids) {
                const p0 = mixPt(me.top[1], me.top[0], acc / total);
                acc += c.half;
                const p1 = mixPt(me.top[1], me.top[0], acc / total);
                fill([p0, p1, c.bottom[1], c.bottom[0]], colour(c.v.n, c.v.depth, 0.62));
            }
        }
        for (const { v, half, top, bottom, base } of sq.values()) {
            const bb = R(Math.min(v.s.x, base.x) - half, Math.min(v.s.y, base.y) - half, Math.max(v.s.x, base.x) + half, Math.max(v.s.y, base.y) + half);
            if (!hits(view, bb)) continue;
            fill([top[0], top[1], bottom[0], bottom[1]], colour(v.n, v.depth, 0.9));
            this.hitSegs.push({ a: base, b: v.s, hw: half, n: v.n });
            segs.push({ a: base, b: v.s, pad: half + 1, box: inflate(bb, 1), p: v.n.parent, c: v.n });
        }
        return segs;
    }

    // ---- hyperbolic camera (Poincaré disk). Node frames are reached from the
    // anchor through the tree by Möbius maps, like the Euclidean camera, so
    // precision does not depend on how deep the anchor is.
    hD() { return this.centre(); }  // the disk's centre on screen
    hDefaultR() { return Math.max(40, Math.min(this.width, this.height - this.insetBottom) * 0.46); }
    hReset() {
        this.hAnim = null;
        const r = this.doc.root;
        this.hcam = { anchor: r ? r.id : '', A: mrot(-PI / 2), R: this.hDefaultR() };
    }
    hFixAnchor() {
        let a = this.doc.find(this.hcam.anchor);
        if (!a || !this.doc.root) return this.hReset();
        while (!this.lay.loc.has(a) && a.parent) a = a.parent;  // climb out of collapsed clades
        this.hcam.anchor = a.id;
    }
    hMatrix(n) {
        const a = this.doc.find(this.hcam.anchor);
        if (!a || !n || !this.lay.loc.has(n) || !this.lay.loc.has(a)) return null;
        let l = a;
        while (!contains(l, n)) l = l.parent;
        let m = this.hcam.A;
        for (let x = a; x !== l; x = x.parent) m = mmul(m, minv(this.lay.hyp.get(x)));
        const path = [];
        for (let x = n; x !== l; x = x.parent) path.push(x);
        for (let i = path.length - 1; i >= 0; --i) m = mmul(m, this.lay.hyp.get(path[i]));
        return mnormalize(m);
    }
    hFrame(m) {
        return Frame(add(this.hD(), scale(mOrigin(m), this.hcam.R)), scale(mDeriv0(m), this.hcam.R * HYP_UNIT));
    }
    // Control point making the drawn branch follow the geodesic (a circle arc).
    hCtrl(pm, L, a, b) {
        // the hyperbolic midpoint of the edge (edges differ in length): distance d/2 is tanh(d/4)
        const c = mOrigin(L), r = abs(c), mid = r > 0 ? scale(c, Math.tanh(Math.atanh(r) / 2) / r) : c;
        const m = add(this.hD(), scale(mapply(pm, mid), this.hcam.R));
        return sub(scale(m, 2), scale(add(a, b), 0.5));
    }
    hComputeItems() {
        const A = this.doc.find(this.hcam.anchor);
        if (!A || !this.lay.loc.has(A)) return;
        const down = (n, m, d, skip) => {
            const f = this.hFrame(m);
            if (!finiteFrame(f) || abs(f.z) < HYP_MIN_PX) return;
            this.items.push(this.makeItem(n, f, d));
            if (hidesKids(n)) return;
            for (const k of n.kids) {
                if (k === skip) continue;
                const L = this.lay.hyp.get(k), mk = mnormalize(mmul(m, L)), cf = this.hFrame(mk);
                if (!finiteFrame(cf) || abs(cf.z) < HYP_MIN_PX) continue;
                this.branches.push({ child: k, pf: f, cf, depth: d, ctrl: this.hCtrl(m, L, f.o, cf.o) });
                down(k, mk, d + 1, null);
            }
        };
        let d = nodeDepth(A), m = this.hcam.A;
        down(A, m, d, null);
        for (let c = A; c.parent; c = c.parent, --d) {
            const L = this.lay.hyp.get(c), pm = mnormalize(mmul(m, minv(L)));
            const pf = this.hFrame(pm), cf = this.hFrame(m);
            if (finiteFrame(pf) && finiteFrame(cf) && abs(cf.z) >= HYP_MIN_PX)
                this.branches.push({ child: c, pf, cf, depth: d - 1, ctrl: this.hCtrl(pm, L, pf.o, cf.o) });
            down(c.parent, pm, d - 1, c);
            m = pm;
        }
        this.items.sort((a, b) => a.depth - b.depth);
    }
    // Slide the plane so the point under `from` follows the pointer to `to`.
    hPan(from, to) {
        const r = this.hcam.R, D = this.hD();
        const disk = p => { const u = scale(sub(p, D), 1 / r), k = abs(u); return k > 0.97 ? scale(u, 0.97 / k) : u; };
        const u0 = disk(from), u1 = disk(to);
        this.hcam.A = mnormalize(mmul(mmul(minv(mtoZero(u1)), mtoZero(u0)), this.hcam.A));
        this.hReanchor();
    }
    // Keep the anchor at the node nearest the centre of the disk.
    hReanchor() {
        let a = this.doc.find(this.hcam.anchor);
        if (!a || !this.lay.loc.has(a)) return;
        let A = this.hcam.A;
        for (let guard = 0; guard < 10000; ++guard) {
            let best = null, bestM = null, bw = abs(mOrigin(A)) - 1e-9;
            const consider = (n, m) => { const w = abs(mOrigin(m)); if (w < bw) { bw = w; best = n; bestM = m; } };
            if (a.parent) consider(a.parent, mmul(A, minv(this.lay.hyp.get(a))));
            if (!hidesKids(a)) for (const k of a.kids) consider(k, mmul(A, this.lay.hyp.get(k)));
            if (!best) break;
            a = best;
            A = mnormalize(bestM);
        }
        this.hcam.anchor = a.id;
        this.hcam.A = A;
    }
    // Glide n to the centre along the geodesic, turning it to grow upwards.
    hFocus(n) {
        const m = this.hMatrix(n);
        if (!m) return;
        const q = mOrigin(m), end = mmul(mtoZero(q), m);
        const turn = remainder(-PI / 2 - arg(mDeriv0(end)), 2 * PI);
        this.hAnim = { start: performance.now(), A0: this.hcam.A, q, turn };
        this.loop();
    }

    // Jump straight to the view focusOn would fly to.
    focusNow(n) {
        if (!n || !this.lay.loc.has(n)) return;
        if (this.hyp()) {
            this.hcam = { anchor: n.id, A: mrot(-PI / 2), R: this.hDefaultR() };
            return;
        }
        let mag = Math.min(this.width, this.height) * 0.42;
        if (!this.disks()) {
            const f = this.screenFrame(n);
            mag = Math.min(Math.max(f ? abs(f.z) : 120, 120), mag / this.lay.extent(n));
        }
        this.camAnim = null;
        this.cam.anchor = n.id;
        this.cam.f = Frame(this.centre(), polar(mag, this.absAngle(n)));
        this.reanchor();
    }

    // ---- morphing between layouts, in screen space. A snapshot records, for
    // every visible node, its frame, node size and the branch into it as drawn.
    snapshot() {
        const st = this.lay.style, snap = new Map();
        snap.hyp = st === HYPERBOLIC;
        const bw = (st === PYTHAGORAS) ? 2 * PY_W : STD_WIDTH;
        for (const v of this.items)
            snap.set(v.n.id, {
                n: v.n, parent: v.n.parent ? v.n.parent.id : '', o: v.s, z: v.f.z, r: v.r, lenPx: v.lenPx, angle: v.angle,
                depth: v.depth, branchPx: v.branchPx, nodeA: st === PYTHAGORAS ? 0 : 1, fillB: st === DISKS ? 0.55 : 1,
                lineB: st === DISKS ? 1 : 0.65, inc: null,
            });
        for (const br of this.branches) {
            const it = snap.get(br.child.id);
            if (!it) continue;
            const a = br.pf.o, b = br.cf.o;
            const ctrl = st === PYTHAGORAS ? scale(add(a, b), 0.5)
                       : br.ctrl || add(a, scale(br.pf.z, abs(sub(b, a)) * STD_CURVE / abs(br.pf.z)));
            const wb = bw * abs(br.cf.z), wa = st === PYTHAGORAS ? wb : bw * abs(br.pf.z);
            it.inc = { a, c: ctrl, b, wa, wb };
        }
        return snap;
    }

    startMorph(from, ms = MORPH_MS) {
        this.computeItems();
        this.morph = { start: performance.now(), ms, from, to: this.snapshot(), fromHyp: from.hyp };
        this.loop();
    }

    renderMorph() {
        const ctx = this.ctx, { from, to, start, ms } = this.morph;
        const e = easeInOutCubic(clamp((performance.now() - start) / ms, 0, 1));
        const lin = (a, b) => a + (b - a) * e, mix = (p, q) => C(lin(p.x, q.x), lin(p.y, q.y));
        const geo = (a, b) => Math.exp(lin(Math.log(Math.max(a, 1e-6)), Math.log(Math.max(b, 1e-6))));
        // A node missing on one side grows from (or shrinks into) its nearest ancestor there.
        const stand = (snap, other) => {
            for (let id = other.parent; id; ) {
                const a = snap.get(id);
                if (a) return { ...other, o: a.o, z: scale(a.z, 0.02), r: 0.5, lenPx: 1, nodeA: 0, inc: null, ghost: true };
                const n = this.doc.find(id) || (from.get(id) || to.get(id) || {}).n;
                id = n && n.parent ? n.parent.id : '';
            }
            return null;
        };
        const ids = new Set([...from.keys(), ...to.keys()]);
        const items = [], inc = [];
        for (const id of ids) {
            let f = from.get(id), t = to.get(id);
            if (!f) f = stand(from, t);
            if (!t) t = stand(to, f);
            if (!f || !t) continue;
            const n = this.doc.find(id) || t.n || f.n;
            const o = mix(f.o, t.o), z = lerpZ(f.z, t.z, e, true);
            const it = { n, f: Frame(o, z), s: o, r: geo(f.r, t.r), lenPx: geo(f.lenPx, t.lenPx),
                         angle: f.angle + remainder(t.angle - f.angle, 2 * PI) * e, depth: t.depth, branchPx: geo(f.branchPx, t.branchPx),
                         nodeA: lin(f.nodeA, t.nodeA), fillB: lin(f.fillB, t.fillB), lineB: lin(f.lineB, t.lineB) };
            items.push(it);
            if (f.inc || t.inc) {  // a side without the branch draws it at zero width
                const fi = f.inc || { a: f.o, c: f.o, b: f.o, wa: 0, wb: 0 }, ti = t.inc || { a: t.o, c: t.o, b: t.o, wa: 0, wb: 0 };
                inc.push({ n, depth: t.depth - 1, a: mix(fi.a, ti.a), c: mix(fi.c, ti.c), b: mix(fi.b, ti.b),
                           wa: lin(fi.wa, ti.wa), wb: lin(fi.wb, ti.wb), alpha: lin(f.inc ? 1 : 0, t.inc ? 1 : 0) });
            }
        }
        items.sort((a, b) => a.depth - b.depth);
        const hypA = (this.morph.fromHyp ? 1 - e : 0) + (this.hyp() ? e : 0);
        if (hypA > 0) this.drawDisk(hypA);
        const segs = [];
        for (const br of inc) {
            if (br.alpha <= 0.01 || (br.wa < 0.05 && br.wb < 0.05)) continue;
            ctx.globalAlpha = br.alpha;
            const pts = taper(br.a, br.c, br.b, Math.max(0.6, br.wa), Math.max(0.6, br.wb));
            ctx.beginPath();
            pts.forEach((p, i) => i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y));
            ctx.closePath();
            ctx.fillStyle = css(this.inCut(br.n) ? darker(this.theme.cut, 140) : this.depthColor(br.depth, 0.7));
            ctx.fill();
            const pad = Math.max(br.wa, br.wb) / 2 + 1;
            segs.push({ a: br.a, b: br.b, pad, box: inflate(R(Math.min(br.a.x, br.b.x), Math.min(br.a.y, br.b.y), Math.max(br.a.x, br.b.x), Math.max(br.a.y, br.b.y)), pad), p: br.n.parent, c: br.n });
        }
        for (const v of items) {
            if (v.nodeA <= 0.01) continue;
            ctx.globalAlpha = v.nodeA;
            if (v.r > 1e5 || this.covers(v.s, v.r)) {
                if (this.covers(v.s, v.r)) { ctx.fillStyle = css(this.depthColor(v.depth, v.fillB)); ctx.fillRect(0, 0, this.width, this.height); }
                continue;
            }
            ctx.beginPath();
            ctx.arc(v.s.x, v.s.y, v.r, 0, 2 * PI);
            ctx.fillStyle = css(this.inCut(v.n) ? darker(this.theme.cut, 160) : this.depthColor(v.depth, v.fillB));
            ctx.fill();
            ctx.lineWidth = clamp(v.r * 0.05, 1, 3);
            ctx.strokeStyle = css(this.depthColor(v.depth, v.lineB));
            ctx.stroke();
        }
        ctx.globalAlpha = 1;
        const sel = this.doc.find(this.selId);
        for (const v of items)
            if (v.n === sel && v.r < 1e5) {
                ctx.lineWidth = 2;
                ctx.strokeStyle = css(this.theme.accent);
                ctx.beginPath(); ctx.arc(v.s.x, v.s.y, v.r + 4, 0, 2 * PI); ctx.stroke();
            }
        this.items = items;
        this.drawLabels(segs, false);
    }

    // ---- hit testing
    hitTest(p, slop = 5) {
        const label = () => {
            for (let i = this.labelHits.length - 1; i >= 0; --i)
                if (has(this.labelHits[i][0], p)) { const n = this.doc.find(this.labelHits[i][1]); if (n) return n; }
            return null;
        };
        let best = null;
        if (this.disks()) {
            const l = label();
            if (l) return l;
            let br = Infinity;
            for (const it of this.items) if (it.r < br && Math.hypot(it.s.x - p.x, it.s.y - p.y) <= it.r) { best = it.n; br = it.r; }
            return best;
        }
        let bd = Infinity, exact = false;
        for (const it of this.items) {
            const d = Math.hypot(it.s.x - p.x, it.s.y - p.y);
            if (d <= it.r + slop && d < bd) { best = it.n; bd = d; exact = d <= it.r; }
        }
        if (exact) return best;
        const l = label();
        if (l) return l;
        if (best) return best;
        let bsd = Math.max(3, slop - 2);
        for (const s of this.hitSegs) { const d = segDist(p, s.a, s.b) - s.hw; if (d < bsd) { bsd = d; best = s.n; } }
        return best;
    }

    // ---- input: mouse, pen and touch through pointer events
    bindInput() {
        const cv = this.canvas;
        const pos = e => { const r = cv.getBoundingClientRect(); return C(e.clientX - r.left, e.clientY - r.top); };
        cv.addEventListener('contextmenu', e => e.preventDefault());

        cv.addEventListener('pointerdown', e => {
            try { cv.setPointerCapture(e.pointerId); } catch { /* synthetic or already-ended pointer */ }
            const p = pos(e);
            this.pointers.set(e.pointerId, p);
            this.h.focus?.();
            if (this.pointers.size === 1) {
                this.press = { p, last: p, button: e.button, type: e.pointerType, moved: false, long: false };
                clearTimeout(this.longTimer);
                if (e.pointerType !== 'mouse')
                    this.longTimer = setTimeout(() => {
                        if (!this.press || this.press.moved) return;
                        this.press.long = true;
                        const hit = this.hitTest(this.press.p, 14);
                        navigator.vibrate?.(10);
                        this.h.menu(hit ? hit.id : '', e.clientX, e.clientY);
                    }, LONG_PRESS_MS);
            } else if (this.pointers.size === 2) {
                clearTimeout(this.longTimer);
                if (this.press) this.press.moved = true;
                const [a, b] = [...this.pointers.values()];
                this.pinch = { dist: Math.hypot(a.x - b.x, a.y - b.y), mid: C((a.x + b.x) / 2, (a.y + b.y) / 2) };
            }
        });

        cv.addEventListener('pointermove', e => {
            const p = pos(e);
            if (!this.pointers.has(e.pointerId)) {
                if (e.pointerType === 'mouse') this.updateHover(p);
                return;
            }
            this.pointers.set(e.pointerId, p);
            if (this.pointers.size >= 2 && this.pinch) {
                const [a, b] = [...this.pointers.values()];
                const dist = Math.hypot(a.x - b.x, a.y - b.y), mid = C((a.x + b.x) / 2, (a.y + b.y) / 2);
                this.following = false;
                if (this.pinch.dist > 0 && dist > 0) this.zoomBy(dist / this.pinch.dist, mid);
                this.panBy(sub(mid, this.pinch.mid), mid);
                this.pinch = { dist, mid };
                this.viewUpdated();
                return;
            }
            const pr = this.press;
            if (!pr) return;
            const slop = pr.type === 'mouse' ? 4 : 10;
            if (!pr.moved && Math.hypot(p.x - pr.p.x, p.y - pr.p.y) > slop) { pr.moved = true; clearTimeout(this.longTimer); }
            if (pr.moved && !pr.long) {
                this.following = false;
                this.panBy(sub(p, pr.last), p);
                this.viewUpdated();
            }
            pr.last = p;
        });

        const end = e => {
            if (!this.pointers.has(e.pointerId)) return;
            this.pointers.delete(e.pointerId);
            if (this.pointers.size === 1) {  // pinch -> one finger: continue panning from it
                const q = [...this.pointers.values()][0];
                if (this.press) this.press.last = q;
                this.pinch = null;
                return;
            }
            if (this.pointers.size) return;
            this.pinch = null;
            clearTimeout(this.longTimer);
            const pr = this.press;
            this.press = null;
            if (!pr || pr.moved || pr.long || e.type === 'pointercancel') return;
            const p = pos(e), touch = pr.type !== 'mouse';
            const hit = this.hitTest(p, touch ? 14 : 5), id = hit ? hit.id : '';
            if (pr.button === 2) return this.h.menu(id, e.clientX, e.clientY);
            if (pr.button !== 0) return;
            const now = performance.now(), lt = this.lastTap;
            const dbl = now - lt.t < DOUBLE_TAP_MS && Math.hypot(p.x - lt.x, p.y - lt.y) < 24 && id && lt.id === id;
            this.lastTap = dbl ? { t: 0, x: 0, y: 0 } : { t: now, x: p.x, y: p.y, id };
            if (dbl) this.h.edit(id);
            else this.h.select(id);
            if (!touch) this.updateHover(p);
        };
        cv.addEventListener('pointerup', end);
        cv.addEventListener('pointercancel', end);
        cv.addEventListener('pointerleave', e => { if (e.pointerType === 'mouse' && this.hoverId) { this.hoverId = ''; this.draw(); } });

        cv.addEventListener('wheel', e => {
            e.preventDefault();
            const p = pos(e);
            let f;
            if (e.ctrlKey) f = Math.exp(-e.deltaY * 0.01);  // trackpad pinch
            else {
                const fast = e.shiftKey;
                let d = e.deltaY || (fast ? e.deltaX : 0);
                d *= e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? 800 : 1;
                f = Math.pow(fast ? FAST_ZOOM_STEP : ZOOM_STEP, -d / 100);
            }
            this.following = false;
            this.zoomBy(f, p);
            this.updateHover(p);
            this.viewUpdated();
        }, { passive: false });
    }

    updateHover(p) {
        const h = this.hitTest(p);
        this.canvas.style.cursor = h ? 'pointer' : '';
        const id = h ? h.id : '';
        if (id !== this.hoverId) { this.hoverId = id; this.draw(); }
    }
}
