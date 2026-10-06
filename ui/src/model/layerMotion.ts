// Layers that travel along their own paths: where a layer's path lives in the
// selection model, keeping the path attached to the layer, when a layer
// arrives at the end of its path, and "fit": making the listener and chosen
// layers start and finish together.
import { defaultMotion, firstPoint, hasPath, speedArrival, type LayerDoc, type PathDoc, type SceneDoc, type SegmentDoc, type V3 } from './scene';
import { appendSegments, round3, samplePath, translatePath } from './geometry';
import type { Analysis } from './store';

// Point selection and handles name a path by index: listener paths are 0, 1,
// 2 ...; layer i's path is -1 - i.
export const layerPathIndex = (layer: number): number => -1 - layer;
export const layerOfPath = (path: number): number => (path < 0 ? -1 - path : -1);

export function pathOf(scene: SceneDoc, index: number): PathDoc | undefined {
  if (index >= 0) return scene.listener.paths[index];
  const l = scene.layers[layerOfPath(index)];
  return l && hasPath(l) ? l.motion!.path : undefined;
}

// Moves a layer to `to`; its path goes with it.
export function moveLayerTo(l: LayerDoc, to: V3): void {
  const d: V3 = [to[0] - l.position[0], to[1] - l.position[1], to[2] - l.position[2]];
  l.position = round3(to);
  if (l.motion) translatePath(l.motion.path, d);
}

// Gives the layer a drawn path (or adds the segments to its open path when
// `append`). A new path is lifted or lowered to the layer's height and the
// layer moves to where it begins, so the line drawn is the line travelled.
export function attachPath(l: LayerDoc, segs: SegmentDoc[], closed: boolean, append: boolean, startTime: number): void {
  if (append && l.motion && hasPath(l) && !l.motion.path.closed) {
    const end = samplePath(l.motion.path).at(-1);
    const first = segs[0] && (segs[0].type === 'arc' ? segs[0].points[1] : segs[0].points[0]);
    const path: PathDoc = { name: '', closed: false, segments: segs };
    if (end && first) translatePath(path, [0, end[1] - first[1], 0]);
    appendSegments(l.motion.path, path.segments);
    l.motion.path.closed = closed;
    return;
  }
  const path: PathDoc = { name: l.name, closed, segments: segs };
  const f = firstPoint(path);
  if (!f) return;
  translatePath(path, [0, l.position[1] - f[1], 0]);
  const start = firstPoint(path)!;
  const prev = l.motion;
  l.motion = { ...defaultMotion(path), ...(prev ? { timing: prev.timing, speed: prev.speed, end: prev.end, turn: prev.turn, keys: prev.keys, fraction: prev.fraction } : {}), path, start_time: prev?.start_time ?? startTime };
  l.position = round3(start);
  path.home = [...l.position] as V3;
}

export function removePath(l: LayerDoc): void { delete l.motion; }

// Length of a path: the engine's when the analysis has it, else the drawn
// curve's.
export function pathLength(path: PathDoc): number {
  const p = samplePath(path, 64);
  let d = 0;
  for (let i = 1; i < p.length; i++) d += Math.hypot(p[i][0] - p[i - 1][0], p[i][1] - p[i - 1][1], p[i][2] - p[i - 1][2]);
  return d;
}

export function layerPathLength(scene: SceneDoc, analysis: Analysis | null, i: number): number {
  const l = scene.layers[i];
  if (!l || !hasPath(l)) return 0;
  return analysis?.layers?.find((x) => x.layer === i)?.length ?? pathLength(l.motion!.path);
}

export function listenerPathLength(scene: SceneDoc, analysis: Analysis | null): number {
  const L = scene.listener;
  const p = L.paths[L.active_path];
  if (!p) return 0;
  return analysis?.paths[L.active_path]?.length ?? pathLength(p);
}

// When layer i first reaches the end of its path (absolute seconds), or null
// (no path, it never gets there, or it is placed by position).
export function layerArrival(l: LayerDoc, length: number): number | null {
  const m = l.motion;
  if (!m || !hasPath(l)) return null;
  if (m.timing === 'speed') {
    const a = speedArrival(m.speed, length);
    return a === null ? null : m.start_time + a;
  }
  if (m.timing === 'keys' && m.keys.length >= 2) return m.keys[m.keys.length - 1].time;
  return null;
}

export function layerStart(l: LayerDoc): number | null {
  const m = l.motion;
  if (!m || !hasPath(l)) return null;
  if (m.timing === 'speed') return m.start_time;
  if (m.timing === 'keys' && m.keys.length) return m.keys[0].time;
  return null;
}

const r3 = (v: number) => Math.round(v * 1000) / 1000;
const r4 = (v: number) => Math.round(v * 10000) / 10000;

// Scales a speed curve in time by f (key times x f, speeds / f): the same
// shape of journey, covering the same distance, f times as long.
export function stretchSpeed(keys: { time: number; speed: number }[], f: number): void {
  if (!(f > 0) || !Number.isFinite(f)) return;
  for (const k of keys) { k.time = r3(k.time * f); k.speed = r4(k.speed / f); }
}

// Makes a speed-timed journey of `length` metres start at `start` and end at
// `end`: the curve's shape stretched or squeezed to fit, or a steady speed
// when the curve never arrives (it stops on the way).
function fitSpeed(keys: { time: number; speed: number; easing: string }[], length: number, start: number, end: number): void {
  const span = Math.max(0.1, end - start);
  const a = speedArrival(keys as never, length);
  if (a !== null && a > 1e-6) { stretchSpeed(keys, span / a); return; }
  keys.splice(0, keys.length, { time: 0, speed: r4(length / span), easing: 'linear' });
}

export interface FitTarget { listener: boolean; layers: number[] }

// Fit: the listener (on its active path, moving by speed) and the chosen
// layers all set off at `start` and reach the end of their paths at `end`.
// Speed-timed journeys keep their shape (stretched or squeezed); key-timed
// layers have their keys spread over the same span.
export function fitTiming(scene: SceneDoc, analysis: Analysis | null, target: FitTarget, start: number, end: number): void {
  start = Math.max(0, start);
  if (end <= start) return;
  const L = scene.listener;
  if (target.listener && L.paths[L.active_path] && L.position_mode === 'speed') {
    L.path_start_time = r3(start);
    fitSpeed(L.speed, listenerPathLength(scene, analysis), start, end);
  }
  for (const i of target.layers) {
    const l = scene.layers[i];
    const m = l?.motion;
    if (!m || !hasPath(l)) continue;
    if (m.timing === 'speed') {
      m.start_time = r3(start);
      fitSpeed(m.speed, layerPathLength(scene, analysis, i), start, end);
    } else if (m.timing === 'keys') {
      if (m.keys.length < 2) {
        m.keys = [{ time: r3(start), fraction: 0, easing: 'smooth' }, { time: r3(end), fraction: 1, easing: 'smooth' }];
        continue;
      }
      const t0 = m.keys[0].time, t1 = m.keys[m.keys.length - 1].time;
      for (const k of m.keys) k.time = r3(t1 > t0 ? start + (k.time - t0) / (t1 - t0) * (end - start) : start);
    }
  }
}

// Layers whose timing fit can change (speed or keys, with a path).
export function fittableLayers(scene: SceneDoc): number[] {
  return scene.layers.map((l, i) => (hasPath(l) && l.motion!.timing !== 'position' ? i : -1)).filter((i) => i >= 0);
}
