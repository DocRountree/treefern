// Complex numbers as {x, y}, plus the parent-relative frames the layout and
// camera are built from (same maths as the desktop app).

export const PI = Math.PI, DEG = PI / 180;

export const C = (x, y = 0) => ({ x, y });
export const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y });
export const sub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y });
export const mul = (a, b) => ({ x: a.x * b.x - a.y * b.y, y: a.x * b.y + a.y * b.x });
export const div = (a, b) => {
    const d = b.x * b.x + b.y * b.y;
    return { x: (a.x * b.x + a.y * b.y) / d, y: (a.y * b.x - a.x * b.y) / d };
};
export const scale = (a, s) => ({ x: a.x * s, y: a.y * s });
export const abs = a => Math.hypot(a.x, a.y);
export const norm = a => a.x * a.x + a.y * a.y;
export const arg = a => Math.atan2(a.y, a.x);
export const polar = (r, t) => ({ x: r * Math.cos(t), y: r * Math.sin(t) });

// IEEE remainder: x - n*y with n the nearest integer to x/y.
export const remainder = (x, y) => x - Math.round(x / y) * y;
export const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

// A node's frame relative to its parent's: origin o and scale+rotation z (both
// in parent units); dir = growth direction in its own frame.
export const Local = (o = C(0, 0), z = C(1, 0), dir = -PI / 2) => ({ o, z, dir });
// A frame on screen: local point q maps to pixel o + z*q.
export const Frame = (o = C(0, 0), z = C(1, 0)) => ({ o, z });

export const compose = (f, l) => Frame(add(f.o, mul(f.z, l.o)), mul(f.z, l.z));
export function uncompose(f, l) {
    const pz = div(f.z, l.z);
    return Frame(sub(f.o, mul(pz, l.o)), pz);
}
export const finiteFrame = f =>
    [f.o.x, f.o.y, f.z.x, f.z.y].every(Number.isFinite) && abs(f.z) > 0;

export const lerpAngle = (a, b, t) => a + remainder(b - a, 2 * PI) * t;
// Rotate along the shorter arc; magnitude linearly, or geometrically for camera zoom.
export function lerpZ(a, b, t, geometric = false) {
    const ma = abs(a), mb = abs(b);
    return polar(geometric ? ma * Math.pow(mb / ma, t) : ma + (mb - ma) * t, lerpAngle(arg(a), arg(b), t));
}
export const lerpLocal = (a, b, t) =>
    Local(add(a.o, scale(sub(b.o, a.o), t)), lerpZ(a.z, b.z, t), lerpAngle(a.dir, b.dir, t));

export const easeOutCubic = t => 1 - Math.pow(1 - t, 3);
export const easeInOutCubic = t => t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;

// ---- Möbius maps of the unit disk, as 2x2 complex matrices {a, b, c, d}:
// z -> (a z + b) / (c z + d). Used by the hyperbolic (Poincaré disk) view.
export const conj = a => ({ x: a.x, y: -a.y });
export const M = (a, b, c, d) => ({ a, b, c, d });
export const mmul = (p, q) => M(add(mul(p.a, q.a), mul(p.b, q.c)), add(mul(p.a, q.b), mul(p.b, q.d)),
                                add(mul(p.c, q.a), mul(p.d, q.c)), add(mul(p.c, q.b), mul(p.d, q.d)));
export const mapply = (m, z) => div(add(mul(m.a, z), m.b), add(mul(m.c, z), m.d));
export const minv = m => M(m.d, scale(m.b, -1), scale(m.c, -1), m.a);  // for determinant 1
export const MIDENT = M(C(1), C(0), C(0), C(1));
// Rotation by phi about the centre.
export const mrot = phi => M(polar(1, phi / 2), C(0), C(0), polar(1, -phi / 2));
// Hyperbolic translation along +x by distance d: 0 -> tanh(d/2).
export const mtransX = d => M(C(Math.cosh(d / 2)), C(Math.sinh(d / 2)), C(Math.sinh(d / 2)), C(Math.cosh(d / 2)));
// The translation taking point c of the disk to 0.
export function mtoZero(c) {
    const k = 1 / Math.sqrt(Math.max(1e-300, 1 - norm(c)));
    return M(C(k), scale(c, -k), scale(conj(c), -k), C(k));
}
// Image of 0 and the derivative there (a local similarity: scale + rotation).
export const mOrigin = m => div(m.b, m.d);
export const mDeriv0 = m => div(sub(mul(m.a, m.d), mul(m.b, m.c)), mul(m.d, m.d));
// Rescale to determinant 1 so long products do not drift.
export function mnormalize(m) {
    const det = sub(mul(m.a, m.d), mul(m.b, m.c));
    const r = Math.sqrt(abs(det)), t = arg(det) / 2;
    const s = div(C(1), polar(r, t));
    return M(mul(m.a, s), mul(m.b, s), mul(m.c, s), mul(m.d, s));
}
