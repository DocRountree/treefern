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
