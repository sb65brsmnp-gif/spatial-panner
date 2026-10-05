// Path geometry for the drawing tools: simplification, shape generators,
// point editing on the engine's segment types, and a local evaluator for
// previews. The authoritative curve comes from the engine (analysis); the
// local evaluator mirrors SampledPath::evaluateSegment for instant feedback.
import type { PathDoc, SegmentDoc, V3 } from './scene';

export const add = (a: V3, b: V3): V3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
export const sub = (a: V3, b: V3): V3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
export const scale = (a: V3, s: number): V3 => [a[0] * s, a[1] * s, a[2] * s];
export const len = (a: V3): number => Math.hypot(a[0], a[1], a[2]);
export const dist = (a: V3, b: V3): number => len(sub(a, b));
export const lerp3 = (a: V3, b: V3, t: number): V3 => add(a, scale(sub(b, a), t));
export const same = (a: V3, b: V3, eps = 1e-5): boolean => dist(a, b) < eps;
export const round3 = (a: V3): V3 => a.map((v) => Math.round(v * 1000) / 1000) as V3;

// ------------------------------------------------------------ simplification

function perpendicularDistance(p: V3, a: V3, b: V3): number {
  const ab = sub(b, a);
  const l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
  if (l2 < 1e-12) return dist(p, a);
  const t = Math.max(0, Math.min(1, ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] + (p[2] - a[2]) * ab[2]) / l2));
  return dist(p, add(a, scale(ab, t)));
}

// Ramer-Douglas-Peucker.
export function simplify(points: V3[], epsilon: number): V3[] {
  if (points.length < 3) return points.slice();
  const keep = new Uint8Array(points.length);
  keep[0] = keep[points.length - 1] = 1;
  const stack: [number, number][] = [[0, points.length - 1]];
  while (stack.length) {
    const [i0, i1] = stack.pop()!;
    let maxD = 0, idx = -1;
    for (let i = i0 + 1; i < i1; i++) {
      const d = perpendicularDistance(points[i], points[i0], points[i1]);
      if (d > maxD) { maxD = d; idx = i; }
    }
    if (idx >= 0 && maxD > epsilon) {
      keep[idx] = 1;
      stack.push([i0, idx], [idx, i1]);
    }
  }
  return points.filter((_, i) => keep[i]);
}

// Freehand stroke -> a Catmull-Rom segment through the simplified stroke
// (spec: RDP simplification then a Catmull-Rom fit).
export function fitFreehand(stroke: V3[], epsilon = 0.08): SegmentDoc | null {
  const pts: V3[] = [];
  for (const p of stroke) if (!pts.length || dist(p, pts[pts.length - 1]) > 0.02) pts.push(p);
  if (pts.length < 2) return null;
  const s = simplify(pts, epsilon).map(round3);
  if (s.length < 2 || dist(s[0], s[s.length - 1]) + (s.length > 2 ? 1 : 0) < 0.05) return null;
  return s.length === 2 ? { type: 'line', points: s } : { type: 'catmull_rom', points: s };
}

// ------------------------------------------------------------------- shapes

const K = 0.5522847498;  // cubic Bezier quarter-circle handle length

export function ellipse(c: V3, rx: number, rz: number): SegmentDoc[] {
  const p = (dx: number, dz: number): V3 => round3([c[0] + dx, c[1], c[2] + dz]);
  // Counter-clockwise seen from above, starting in front (-Z) of the centre.
  const a: V3[] = [p(0, -rz), p(-rx, 0), p(0, rz), p(rx, 0)];
  const segs: SegmentDoc[] = [];
  for (let i = 0; i < 4; i++) {
    const s = a[i], e = a[(i + 1) % 4];
    const ts = tangentAt(i), te = tangentAt((i + 1) % 4);
    segs.push({ type: 'bezier', points: [s, round3(add(s, ts)), round3(sub(e, te)), e] });
  }
  return segs;
  function tangentAt(i: number): V3 {
    // Tangent direction (scaled to the handle length) at anchor i going counter-clockwise.
    switch (i) {
      case 0: return [-rx * K, 0, 0];
      case 1: return [0, 0, rz * K];
      case 2: return [rx * K, 0, 0];
      default: return [0, 0, -rz * K];
    }
  }
}

export const circle = (c: V3, r: number): SegmentDoc[] => ellipse(c, r, r);

// Closed uniform Catmull-Rom through `pts` as cubic Beziers (smooth at every
// point, including where the loop closes, which a Catmull-Rom segment plus
// the path's closing line would not be).
export function closedSmooth(pts: V3[]): SegmentDoc[] {
  const n = pts.length;
  const segs: SegmentDoc[] = [];
  for (let i = 0; i < n; i++) {
    const p0 = pts[(i - 1 + n) % n], p1 = pts[i], p2 = pts[(i + 1) % n], p3 = pts[(i + 2) % n];
    segs.push({
      type: 'bezier',
      points: [round3(p1), round3(add(p1, scale(sub(p2, p0), 1 / 6))), round3(sub(p2, scale(sub(p3, p1), 1 / 6))), round3(p2)],
    });
  }
  return segs;
}

export function figure8(c: V3, w: number, d: number): SegmentDoc[] {
  const pts: V3[] = [];
  const n = 12;
  for (let i = 0; i < n; i++) {
    const t = (i / n) * Math.PI * 2;
    pts.push([c[0] + w * Math.sin(t), c[1], c[2] - d * Math.sin(t) * Math.cos(t) * 2]);
  }
  return closedSmooth(pts);
}

export function spiral(c: V3, rOuter: number, turns: number, rInner = 0.15 * rOuter): SegmentDoc[] {
  const perTurn = 16;
  const n = Math.max(2, Math.round(turns * perTurn) + 1);
  const pts: V3[] = [];
  for (let i = 0; i < n; i++) {
    const u = i / (n - 1);
    const a = u * turns * Math.PI * 2;
    const r = rInner + (rOuter - rInner) * u;
    pts.push(round3([c[0] - r * Math.sin(a), c[1], c[2] - r * Math.cos(a)]));
  }
  return [{ type: 'catmull_rom', points: pts }];
}

export function helix(c: V3, r: number, turns: number, rise: number): SegmentDoc[] {
  const perTurn = 16;
  const n = Math.max(2, Math.round(turns * perTurn) + 1);
  const pts: V3[] = [];
  for (let i = 0; i < n; i++) {
    const u = i / (n - 1);
    const a = u * turns * Math.PI * 2;
    pts.push(round3([c[0] - r * Math.sin(a), c[1] + rise * u, c[2] - r * Math.cos(a)]));
  }
  return [{ type: 'catmull_rom', points: pts }];
}

// ------------------------------------------------------ local evaluation

function catmullRom(p0: V3, p1: V3, p2: V3, p3: V3, u: number): V3 {
  const knot = (a: V3, b: V3) => Math.pow(Math.max(dist(a, b), 1e-6), 0.5);
  const t0 = 0, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
  const t = t1 + (t2 - t1) * u;
  const l = (a: V3, b: V3, ta: number, tb: number) => add(scale(a, (tb - t) / (tb - ta)), scale(b, (t - ta) / (tb - ta)));
  const a1 = l(p0, p1, t0, t1), a2 = l(p1, p2, t1, t2), a3 = l(p2, p3, t2, t3);
  const b1 = l(a1, a2, t0, t2), b2 = l(a2, a3, t1, t3);
  return l(b1, b2, t1, t2);
}

export function evaluateSegment(seg: SegmentDoc, u: number): V3 {
  u = Math.min(1, Math.max(0, u));
  const p = seg.points;
  if (!p.length) return [0, 0, 0];
  switch (seg.type) {
    case 'line':
      return p.length < 2 ? p[0] : lerp3(p[0], p[1], u);
    case 'bezier': {
      if (p.length < 4) return p[0];
      const v = 1 - u;
      return add(add(scale(p[0], v * v * v), scale(p[1], 3 * v * v * u)), add(scale(p[2], 3 * v * u * u), scale(p[3], u * u * u)));
    }
    case 'catmull_rom': {
      const n = p.length;
      if (n === 1) return p[0];
      if (n === 2) return lerp3(p[0], p[1], u);
      const f = u * (n - 1);
      const i = Math.min(Math.floor(f), n - 2);
      const lu = f - i;
      const p1 = p[i], p2 = p[i + 1];
      const p0 = i > 0 ? p[i - 1] : add(p1, sub(p1, p2));
      const p3 = i + 2 < n ? p[i + 2] : add(p2, sub(p2, p1));
      return catmullRom(p0, p1, p2, p3, lu);
    }
    case 'arc': {
      if (p.length < 3) return p[0];
      const c = p[0];
      const a = sub(p[1], c), b = sub(p[2], c);
      const ra = len(a), rb = len(b);
      if (ra < 1e-6) return c;
      let n: V3 = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
      if (len(n) < 1e-5) n = [0, 1, 0];
      n = scale(n, 1 / len(n));
      if (seg.clockwise) n = scale(n, -1);
      const ax = scale(a, 1 / ra);
      const ay: V3 = [n[1] * ax[2] - n[2] * ax[1], n[2] * ax[0] - n[0] * ax[2], n[0] * ax[1] - n[1] * ax[0]];
      const bn = scale(b, 1 / Math.max(rb, 1e-9));
      let ang = Math.atan2(bn[0] * ay[0] + bn[1] * ay[1] + bn[2] * ay[2], bn[0] * ax[0] + bn[1] * ax[1] + bn[2] * ax[2]);
      if (ang < 0) ang += Math.PI * 2;
      if (ang < 1e-5 && !(seg.turns ?? 0) && same(p[2], p[1], 1e-5)) ang = Math.PI * 2;
      ang += Math.PI * 2 * Math.max(0, seg.turns ?? 0);
      const th = ang * u, r = ra + (rb - ra) * u;
      return add(c, scale(add(scale(ax, Math.cos(th)), scale(ay, Math.sin(th))), r));
    }
  }
}

export function samplePath(path: PathDoc, perSegment = 48): V3[] {
  const out: V3[] = [];
  for (const seg of path.segments) {
    const n = seg.type === 'line' ? 1 : seg.type === 'catmull_rom' ? perSegment * Math.max(1, seg.points.length - 1) / 4 : perSegment;
    const steps = Math.max(1, Math.round(n));
    for (let i = out.length ? 1 : 0; i <= steps; i++) out.push(evaluateSegment(seg, i / steps));
  }
  if (path.closed && out.length > 1 && !same(out[0], out[out.length - 1], 1e-4)) out.push(out[0]);
  return out;
}

export function pathStart(path: PathDoc): V3 | null {
  return path.segments.length ? evaluateSegment(path.segments[0], 0) : null;
}

// Moves the whole path by `d` (every point, including arc centres and
// Bezier handles), keeping its shape.
export function translatePath(path: PathDoc, d: V3): void {
  for (const seg of path.segments) seg.points = seg.points.map((q) => round3([q[0] + d[0], q[1] + d[1], q[2] + d[2]]));
}

export function pathEnd(path: PathDoc): V3 | null {
  return path.segments.length ? evaluateSegment(path.segments[path.segments.length - 1], 1) : null;
}

// Appends segments to a path, joining with a line when they do not start
// where the path ends.
export function appendSegments(path: PathDoc, segs: SegmentDoc[]): void {
  const end = pathEnd(path);
  if (end && segs.length) {
    const start = evaluateSegment(segs[0], 0);
    if (!same(end, start, 1e-3)) path.segments.push({ type: 'line', points: [round3(end), round3(start)] });
  }
  path.segments.push(...segs);
}

// --------------------------------------------------------- point editing

export interface PointRef { seg: number; pt: number }

export const isHandle = (seg: SegmentDoc, pt: number) => seg.type === 'bezier' && (pt === 1 || pt === 2);

// Every point that sits on the same spot as `ref` and is the shared end of
// neighbouring segments (or the start of a closed loop), so moving one moves
// the joint.
export function jointPoints(path: PathDoc, ref: PointRef): PointRef[] {
  const segs = path.segments;
  const seg = segs[ref.seg];
  const out: PointRef[] = [ref];
  if (!seg || isHandle(seg, ref.pt)) return out;
  const last = seg.points.length - 1;
  const p = seg.points[ref.pt];
  const n = segs.length;
  const consider = (si: number, pi: number) => {
    const s = segs[si];
    if (s && s.points[pi] && same(s.points[pi], p) && !out.some((r) => r.seg === si && r.pt === pi)) out.push({ seg: si, pt: pi });
  };
  if (ref.pt === 0) {
    if (ref.seg > 0) consider(ref.seg - 1, segs[ref.seg - 1].points.length - 1);
    else consider(n - 1, segs[n - 1].points.length - 1);  // loop closing onto the start
  }
  if (ref.pt === last) {
    if (ref.seg < n - 1) consider(ref.seg + 1, 0);
    else consider(0, 0);
  }
  return out;
}

// Moves a point (and its joint and attached Bezier handles) to `to`.
export function movePoint(path: PathDoc, ref: PointRef, to: V3): void {
  const seg = path.segments[ref.seg];
  if (!seg) return;
  const from = seg.points[ref.pt];
  const delta = sub(to, from);
  if (isHandle(seg, ref.pt)) {
    seg.points[ref.pt] = round3(to);
    mirrorSmoothHandle(path, ref, from);
    return;
  }
  for (const r of jointPoints(path, ref)) {
    const s = path.segments[r.seg];
    s.points[r.pt] = round3(add(s.points[r.pt], delta));
    // A Bezier anchor carries its handle.
    if (s.type === 'bezier') {
      const h = r.pt === 0 ? 1 : r.pt === 3 ? 2 : -1;
      if (h >= 0) s.points[h] = round3(add(s.points[h], delta));
    }
  }
}

// When a handle of a smooth anchor moves, the opposite handle follows so the
// curve stays smooth (it keeps its own length).
function mirrorSmoothHandle(path: PathDoc, ref: PointRef, oldPos: V3): void {
  const seg = path.segments[ref.seg];
  const n = path.segments.length;
  const anchorPt = ref.pt === 1 ? 0 : 3;
  const anchor = seg.points[anchorPt];
  let other: PointRef | null = null;
  if (ref.pt === 2) {
    const next = ref.seg < n - 1 ? ref.seg + 1 : 0;
    const s = path.segments[next];
    if (s?.type === 'bezier' && next !== ref.seg && same(s.points[0], anchor)) other = { seg: next, pt: 1 };
  } else {
    const prev = ref.seg > 0 ? ref.seg - 1 : n - 1;
    const s = path.segments[prev];
    if (s?.type === 'bezier' && prev !== ref.seg && same(s.points[3], anchor)) other = { seg: prev, pt: 2 };
  }
  if (!other) return;
  const o = path.segments[other.seg].points[other.pt];
  const oldDir = sub(oldPos, anchor), oDir = sub(o, anchor);
  const lo = len(oDir), lold = len(oldDir);
  if (lo < 1e-6 || lold < 1e-6) return;
  const cos = (oldDir[0] * oDir[0] + oldDir[1] * oDir[1] + oldDir[2] * oDir[2]) / (lo * lold);
  if (cos > -0.996) return;  // was not smooth (more than ~5 degrees off straight)
  const nd = sub(seg.points[ref.pt], anchor);
  const ln = len(nd);
  if (ln < 1e-6) return;
  path.segments[other.seg].points[other.pt] = round3(sub(anchor, scale(nd, lo / ln)));
}

// The editable points of a path, joints listed once.
export function editablePoints(path: PathDoc): PointRef[] {
  const out: PointRef[] = [];
  const seen: PointRef[] = [];
  path.segments.forEach((seg, si) => {
    seg.points.forEach((_, pi) => {
      if (seen.some((r) => r.seg === si && r.pt === pi)) return;
      const j = jointPoints(path, { seg: si, pt: pi });
      seen.push(...j);
      out.push({ seg: si, pt: pi });
    });
  });
  return out;
}

export function getPoint(path: PathDoc, ref: PointRef): V3 | undefined {
  return path.segments[ref.seg]?.points[ref.pt];
}

// Nearest point on the segment to `p`, by sampling then refining.
function nearestOnSegment(seg: SegmentDoc, p: V3): { u: number; d: number } {
  const n = seg.type === 'line' ? 2 : 64;
  let best = { u: 0, d: Infinity };
  for (let i = 0; i <= n; i++) {
    const u = i / n;
    const d = dist(evaluateSegment(seg, u), p);
    if (d < best.d) best = { u, d };
  }
  let step = 1 / n;
  for (let k = 0; k < 12; k++) {
    step /= 2;
    for (const u of [best.u - step, best.u + step]) {
      if (u < 0 || u > 1) continue;
      const d = dist(evaluateSegment(seg, u), p);
      if (d < best.d) best = { u, d };
    }
  }
  return best;
}

export function nearestOnPath(path: PathDoc, p: V3): { seg: number; u: number; d: number } {
  let best = { seg: -1, u: 0, d: Infinity };
  path.segments.forEach((s, i) => {
    const r = nearestOnSegment(s, p);
    if (r.d < best.d) best = { seg: i, ...r };
  });
  return best;
}

// Inserts an editable point where the path passes nearest to `p`.
export function insertPoint(path: PathDoc, p: V3): PointRef | null {
  const hit = nearestOnPath(path, p);
  if (hit.seg < 0) return null;
  const seg = path.segments[hit.seg];
  const at = round3(evaluateSegment(seg, hit.u));
  switch (seg.type) {
    case 'line': {
      const second: SegmentDoc = { type: 'line', points: [at, seg.points[1]] };
      seg.points[1] = at;
      path.segments.splice(hit.seg + 1, 0, second);
      return { seg: hit.seg, pt: 1 };
    }
    case 'catmull_rom': {
      const i = Math.min(Math.floor(hit.u * (seg.points.length - 1)), seg.points.length - 2);
      seg.points.splice(i + 1, 0, at);
      return { seg: hit.seg, pt: i + 1 };
    }
    case 'bezier': {
      // de Casteljau split at u.
      const [p0, p1, p2, p3] = seg.points;
      const u = hit.u;
      const a = lerp3(p0, p1, u), b = lerp3(p1, p2, u), c = lerp3(p2, p3, u);
      const d = lerp3(a, b, u), e = lerp3(b, c, u);
      const m = lerp3(d, e, u);
      seg.points = [p0, round3(a), round3(d), round3(m)];
      path.segments.splice(hit.seg + 1, 0, { type: 'bezier', points: [round3(m), round3(e), round3(c), p3] });
      return { seg: hit.seg, pt: 3 };
    }
    default:
      return null;
  }
}

// Deletes an editable point. Returns false when the path would become degenerate.
export function deletePoint(path: PathDoc, ref: PointRef): boolean {
  const seg = path.segments[ref.seg];
  if (!seg || isHandle(seg, ref.pt)) return false;
  if (seg.type === 'catmull_rom' && ref.pt > 0 && ref.pt < seg.points.length - 1) {
    seg.points.splice(ref.pt, 1);
    if (seg.points.length === 2) seg.type = 'line';
    return true;
  }
  const joints = jointPoints(path, ref);
  const n = path.segments.length;
  // An inner joint between two segments: merge them into one.
  const prev = joints.find((r) => r.seg !== ref.seg && path.segments[r.seg].points.length - 1 === r.pt && r.pt > 0);
  const atEnd = ref.pt === seg.points.length - 1;
  if (atEnd && ref.seg < n - 1) {
    const next = path.segments[ref.seg + 1];
    mergeSegments(path, ref.seg, next);
    return true;
  }
  if (ref.pt === 0 && prev && prev.seg === ref.seg - 1) {
    mergeSegments(path, ref.seg - 1, seg);
    return true;
  }
  // An end of the whole path: drop the end segment (or shorten a Catmull-Rom).
  if (seg.type === 'catmull_rom' && seg.points.length > 2) {
    seg.points.splice(ref.pt, 1);
    if (seg.points.length === 2) seg.type = 'line';
    return true;
  }
  if (n > 1) {
    path.segments.splice(ref.seg, 1);
    return true;
  }
  return false;
}

// Replaces segments i and i+1 (`b`) with one segment from i's start to b's end.
function mergeSegments(path: PathDoc, i: number, b: SegmentDoc): void {
  const a = path.segments[i];
  let merged: SegmentDoc;
  if (a.type === 'bezier' && b.type === 'bezier') {
    merged = { type: 'bezier', points: [a.points[0], a.points[1], b.points[2], b.points[3]] };
  } else if (a.type === 'catmull_rom' && b.type === 'catmull_rom') {
    merged = { type: 'catmull_rom', points: [...a.points.slice(0, -1), ...b.points.slice(1)] };
  } else if (a.type === 'catmull_rom') {
    merged = { type: 'catmull_rom', points: [...a.points.slice(0, -1), b.points[b.points.length - 1]] };
  } else if (b.type === 'catmull_rom') {
    merged = { type: 'catmull_rom', points: [a.points[0], ...b.points.slice(1)] };
  } else {
    merged = { type: 'line', points: [a.points[0], b.points[b.points.length - 1]] };
  }
  if (merged.type === 'catmull_rom' && merged.points.length === 2) merged.type = 'line';
  path.segments.splice(i, 2, merged);
}

export function snap(v: V3, grid: number, keepY = true): V3 {
  if (grid <= 0) return v;
  const s = (x: number) => Math.round(x / grid) * grid;
  return round3([s(v[0]), keepY ? v[1] : s(v[1]), s(v[2])]);
}
