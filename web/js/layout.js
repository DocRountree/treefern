// Tree layout, ported from the desktop app. Every node gets a frame *relative
// to its parent* (origin offset + complex scale/rotation). Children are ordered
// lightest -> heaviest, so the heaviest clade is always the last branch.
import { PI, DEG, C, sub, scale, abs, arg, polar, clamp, Local, Frame, compose } from './geom.js';
import { hidesKids } from './model.js';

export const DEFAULT_SPREAD = 120 * DEG, DEFAULT_SHRINK = 0.68, DEFAULT_CURL = 0.35;
// disks style (nested discs)
const POLY_MARGIN = 0.96;      // usable fraction of a parent's radius
const POLY_GAP = 0.94;         // fraction of its wedge a child fills (both styles)
const POLY_ONLY_CHILD = 0.78;  // radius of a single child
const POLY_EXP = 0.68;         // wedge share = weight^POLY_EXP (both styles)
// standard style
export const STD_NODE = 0.05;   // leaf / collapsed node radius, in its own units
export const STD_WIDTH = 0.09;  // branch width, in units of the frame it leaves
// Labels inside nodes: every node is a disc of this radius, in its own units
// (1 = its outgoing branch), with its name fitted inside. Tune here; above
// about 0.5 a node meets its daughters.
export const INSIDE_NODE = 0.3;
const STD_FAN_STEP = 15 * DEG;  // extra fan angle per child beyond two
const STD_MAX_FAN = 300 * DEG;  // fan cap (unless the spread itself is wider)
const STD_WEIGHT_EXP = 0.35;    // light sisters are scaled by (w / w_max)^this
const STD_BACK_GAP = 25 * DEG;  // keep wedges this far from the incoming stem
const STD_CLEARANCE = 0.2;      // 3+ forks: min distance between sibling subtrees, in fork units

// rectangular and circular cladograms
const CLADO_NODE = 0.3;         // node footprint, in leaf spacings
const CLADO_MIN_STEP = 1.5;     // min distance between levels, in leaf spacings
const RECT_ASPECT = 0.6;        // rectangular: aim for width = this * height

export const STANDARD = 'standard', DISKS = 'disks', RECT = 'rectangular', CIRC = 'circular';
export const STYLES = [STANDARD, DISKS, RECT, CIRC];
export const STYLE_NAMES = { [STANDARD]: 'Standard', [DISKS]: 'Disks', [RECT]: 'Rectangular', [CIRC]: 'Circular' };
export const isClado = st => st === RECT || st === CIRC;

export class Layout {
    constructor() {
        this.style = STANDARD;
        this.spread = DEFAULT_SPREAD;
        this.shrink = DEFAULT_SHRINK;
        this.curl = DEFAULT_CURL;
        this.inside = false;    // standard: labels inside nodes, so nodes are INSIDE_NODE discs
        this.loc = new Map();   // node -> Local (root: relative to the view)
        this.ext = new Map();   // subtree radius around the node's origin, own units
        this.size = new Map();  // node + all descendants (ignores collapse)
        this.leaves = new Map();
        this.bbox = { x0: 0, y0: 0, x1: 0, y1: 0 };
        // cladograms: tips sit at x = tipX (rectangular) or radius tipR around the
        // root (circular), tipSpacing apart; maxTipChars sizes the room for their labels
        this.tipX = this.tipR = 0;
        this.tipSpacing = 1;
        this.maxTipChars = 0;
        this.reach = new Map();  // cladograms: length of the across part of the branch into a node, where its label goes
    }

    count() { return this.loc.size; }
    extent(n) { return this.ext.get(n) ?? 1; }
    weight(n) { return this.size.get(n) ?? 1; }
    own(n) { return this.style === DISKS ? 1 : isClado(this.style) ? CLADO_NODE : this.inside ? INSIDE_NODE : hidesKids(n) ? STD_NODE : STD_WIDTH; }

    compute(root) {
        this.loc.clear(); this.ext.clear(); this.size.clear(); this.leaves.clear(); this.reach.clear();
        this.bbox = { x0: 0, y0: 0, x1: 0, y1: 0 };
        if (!root) return;
        this.weigh(root);
        if (this.style === STANDARD) {
            this.loc.set(root, Local(C(0, 0), C(0, -1), 0));  // grows upwards
            this.standard(root);
        } else if (isClado(this.style)) {
            this.cladogram(root);
        } else {
            this.loc.set(root, Local());
            this.disks(root);
        }
        this.extents(root);
        this.bounds(root);
    }

    // Display order: lightest first, heaviest last; ties broken by visible leaves.
    ordered(n) {
        return n.kids.map((k, i) => [k, i]).sort((a, b) =>
            (this.size.get(a[0]) - this.size.get(b[0])) ||
            (this.leaves.get(a[0]) - this.leaves.get(b[0])) || (a[1] - b[1])).map(p => p[0]);
    }

    weigh(n) {
        let s = 1, l = 0;
        for (const k of n.kids) { this.weigh(k); s += this.size.get(k); l += this.leaves.get(k); }
        this.size.set(n, s);
        this.leaves.set(n, hidesKids(n) ? 1 : l);
    }

    extents(n) {
        let e = this.own(n);
        if (!hidesKids(n))
            for (const k of n.kids) {
                const L = this.loc.get(k);
                e = Math.max(e, abs(L.o) + abs(L.z) * this.extents(k));
            }
        this.ext.set(n, e);
        return e;
    }

    bounds(root) {
        let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
        const addc = (c, r) => {
            x0 = Math.min(x0, c.x - r); x1 = Math.max(x1, c.x + r);
            y0 = Math.min(y0, c.y - r); y1 = Math.max(y1, c.y + r);
        };
        const L = this.loc.get(root);
        if (this.style === STANDARD) addc({ x: L.o.x - L.z.x, y: L.o.y - L.z.y }, 0);  // the trunk below the root
        const stack = [[root, Frame(L.o, L.z)]];
        while (stack.length) {
            const [n, f] = stack.pop();
            addc(f.o, abs(f.z) * this.own(n));
            if (!hidesKids(n)) for (const k of n.kids) stack.push([k, compose(f, this.loc.get(k))]);
        }
        this.bbox = { x0, y0, x1, y1 };
    }

    // Angle of each child's wedge: proportional to weight^POLY_EXP, summing to `total`.
    wedges(kids, total) {
        const th = kids.map(k => Math.pow(this.weight(k), POLY_EXP));
        const sum = th.reduce((a, b) => a + b, 0);
        return th.map(t => t * total / sum);
    }

    // Disks: children sit on a ring inside the parent disc, clockwise from 12
    // o'clock, each the largest disc inscribed in its wedge.
    disks(n) {
        if (hidesKids(n)) return;
        const kids = this.ordered(n);
        if (kids.length === 1) {  // sit low, leaving room for the parent's label
            const r = POLY_MARGIN * POLY_ONLY_CHILD;
            this.loc.set(kids[0], Local(C(0, POLY_MARGIN - r), C(r, 0), PI / 2));
            return this.disks(kids[0]);
        }
        const th = this.wedges(kids, 2 * PI);
        let big = 0;
        th.forEach((t, i) => { if (t > th[big]) big = i; });
        if (th[big] > PI) {  // only one wedge can exceed a half-turn: cap it, rescale the rest
            const rest = 2 * PI - th[big];
            for (let i = 0; i < th.length; ++i) th[i] = i === big ? PI : th[i] * PI / rest;
        }
        const rho = POLY_MARGIN / (1 + Math.sin(th[big] / 2));  // biggest touches the rim
        let a = -PI / 2;
        kids.forEach((k, i) => {
            const mid = a + th[i] / 2;
            this.loc.set(k, Local(polar(rho, mid), C(rho * Math.sin(th[i] / 2) * POLY_GAP, 0), mid));
            this.disks(k);
            a += th[i];
        });
    }

    // Cladograms as drawn in papers. Tips are evenly spaced, one leaf spacing
    // apart, in display order (so the tree is ladderised) and aligned: a node
    // sits as many levels in from the tips as its tallest subtree is deep, and
    // halfway between its first and last daughters. Rectangular: tips in a
    // column on the right. Circular: tips on a circle round the root, from 12
    // o'clock clockwise. Every frame has unit scale and no rotation, so the
    // tree is drawn in one coordinate system and frames are just offsets.
    cladogram(root) {
        const pos = new Map(), height = new Map();
        let tips = 0, chars = 0;
        const visit = n => {
            if (hidesKids(n)) {
                height.set(n, 0);
                pos.set(n, tips++);
                chars = Math.max(chars, n.name.length + (n.collapsed ? 8 : 0));
                return;
            }
            const kids = this.ordered(n);
            let h = 0;
            for (const k of kids) { visit(k); h = Math.max(h, height.get(k) + 1); }
            height.set(n, h);
            pos.set(n, (pos.get(kids[0]) + pos.get(kids.at(-1))) / 2);
        };
        visit(root);
        const H = height.get(root), at = new Map();
        this.maxTipChars = Math.min(chars, 40);
        if (this.style === RECT) {
            const step = H ? Math.max(CLADO_MIN_STEP, RECT_ASPECT * tips / H) : 1;
            this.tipX = H * step;
            this.tipSpacing = 1;
            for (const [n, y] of pos) at.set(n, C((H - height.get(n)) * step, y));
        } else {
            this.tipR = Math.max(CLADO_MIN_STEP * H, tips / (2 * PI));
            this.tipSpacing = 2 * PI * this.tipR / Math.max(tips, 1);
            const step = H ? this.tipR / H : 0;
            for (const [n, i] of pos) at.set(n, polar((H - height.get(n)) * step, -PI / 2 + i * this.tipSpacing / Math.max(this.tipR, 1e-9)));
        }
        for (const [n, p] of at) {
            const q = n.parent && at.get(n.parent);
            this.loc.set(n, Local(q ? sub(p, q) : p, C(1, 0), 0));
            if (q) this.reach.set(n, this.style === RECT ? p.x - q.x : abs(p) - abs(q));
        }
    }

    // Standard: a node's frame has its origin at its fork, +x along its growth
    // direction, and unit = its outgoing branch length. Returns the subtree's
    // bounding circle (centre c on its x axis, radius R) and its pieces.
    standard(n) {
        const own = this.own(n);
        if (hidesKids(n)) return { c: 0, R: own, pieces: [[C(0, 0), own]] };
        const kids = this.ordered(n), m = kids.length;
        const sub = kids.map(k => this.standard(k));
        const circles = [[C(0, 0), own]];
        const placeAt = (i, mid, s) => {
            const u = polar(1, mid);
            this.loc.set(kids[i], Local(u, scale(u, s), 0));
            circles.push([scale(u, 1 + s * sub[i].c), s * sub[i].R]);
        };
        const place = (i, mid, halfWedge, want) => {
            const { c, R } = sub[i];
            const sb = Math.sin(Math.min(halfWedge, PI / 2)) * POLY_GAP;
            placeAt(i, mid, R > c * sb ? Math.min(want, sb / (R - c * sb)) : want);  // inscribed in the wedge
        };
        const shiftFor = (fan, imbalance) => {
            const room = Math.max(0, PI - fan / 2 - STD_BACK_GAP);
            return clamp(this.curl, -1, 1) * imbalance * room;
        };
        if (m === 1) {
            place(0, shiftFor(PI, 1), PI / 2, Math.sqrt(this.shrink));
        } else {
            const fan = Math.min(Math.max(this.spread, STD_MAX_FAN), this.spread + (m - 2) * STD_FAN_STEP);
            const wmax = this.weight(kids[m - 1]);
            const want = [];
            let rest = 0;
            for (let i = 0; i < m; ++i) {
                want[i] = i === m - 1 ? Math.sqrt(this.shrink)  // main stem
                                      : this.shrink * Math.pow(this.weight(kids[i]) / wmax, STD_WEIGHT_EXP);
                if (i + 1 < m) rest += this.weight(kids[i]);
            }
            const imbalance = Math.max(0, (wmax - rest) / (wmax + rest));  // 0 = balanced fork
            let a = -fan / 2 + shiftFor(fan, imbalance);
            if (m === 2) {
                const th = this.wedges(kids, fan);
                for (let i = 0; i < m; ++i) { place(i, a + th[i] / 2, th[i] / 2, want[i]); a += th[i]; }
            } else {
                const pad = Math.min(STD_CLEARANCE / 2, Math.sin(0.9 * fan / (2 * m)));
                const sp = new Array(m);
                const total = k => {  // fan used when every child is scaled by k
                    let t = 0;
                    for (let i = 0; i < m; ++i) {
                        sp[i] = angularSpan(sub[i].pieces, k * want[i], pad);
                        t += sp[i][1] - sp[i][0];
                    }
                    return t;
                };
                let k = 1;
                if (total(1) > fan) {
                    let lo = 1e-3, hi = 1;
                    for (let it = 0; it < 40; ++it) {
                        const mk = (lo + hi) / 2;
                        if (total(mk) <= fan) lo = mk; else hi = mk;
                    }
                    k = lo;
                }
                const used = total(k);  // also leaves sp[] for this k
                const gap = Math.max(0, fan - used) / (m - 1);
                for (let i = 0; i < m; ++i) {
                    placeAt(i, a - sp[i][0], k * want[i]);
                    a += sp[i][1] - sp[i][0] + gap;
                }
            }
        }
        const [x, r] = enclose(circles);
        return { c: x, R: r, pieces: circles };
    }
}

// Angular range, relative to the child's axis and seen from the parent's fork,
// covered by a child at distance 1 with scale s, every piece grown by pad.
function angularSpan(pieces, s, pad) {
    let lo = Infinity, hi = -Infinity;
    for (const [p, r] of pieces) {
        const q = { x: 1 + s * p.x, y: s * p.y };
        const d = abs(q), rr = s * r + pad;
        if (rr >= d * 0.999) return [-PI, PI];
        const a = arg(q), h = Math.asin(rr / d);
        lo = Math.min(lo, a - h);
        hi = Math.max(hi, a + h);
    }
    return hi - lo >= PI ? [-PI, PI] : [lo, hi];
}

// Smallest circle centred on the real axis that encloses all the circles.
function enclose(cs) {
    let lo = Infinity, hi = -Infinity;
    for (const [p, r] of cs) { lo = Math.min(lo, p.x - r); hi = Math.max(hi, p.x + r); }
    const f = x => {
        let m = 0;
        for (const [p, r] of cs) m = Math.max(m, Math.hypot(p.x - x, p.y) + r);
        return m;
    };
    for (let it = 0; it < 60; ++it) {
        const a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
        if (f(a) < f(b)) hi = b; else lo = a;
    }
    const x = (lo + hi) / 2;
    return [x, f(x)];
}
