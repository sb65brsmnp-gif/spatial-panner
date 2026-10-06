// Editor state: the scene document, selection, undo/redo, and the current
// analysis (engine-sampled paths and poses). Views subscribe to changes.
import { completeScene, defaultScene, type SceneDoc } from './scene';
import type { PointRef } from './geometry';
import { pathOf } from './layerMotion';

export interface Analysis {
  duration: number;
  dt: number;
  active_path: number;
  arrival_time: number;
  paths: { length: number; points: [number, number, number][] }[];
  // [x, y, z, yaw, pitch, roll, distance, speed]
  poses: number[][];
  // A mesh room's triangles, for drawing.
  // Layers with paths: [x, y, z, yaw, distance] at t = 0, dt, 2 dt ... (each
  // layer's own dt).
  layers?: { layer: number; length: number; points: [number, number, number][]; dt: number; samples: number[][] }[];
  room_mesh?: { vertices: [number, number, number][]; triangles: [number, number, number][] };
  revision?: number;
  error?: string;
}

export type Selection =
  | { kind: 'none' }
  | { kind: 'layer'; index: number }
  | { kind: 'point'; path: number; ref: PointRef };

export type ChangeKind = 'scene' | 'selection' | 'analysis' | 'time' | 'transport' | 'meters' | 'file' | 'tool';

type Listener = (kinds: Set<ChangeKind>) => void;

export interface AudioInfo { path: string; name: string; duration: number; channels: number; sampleRate: number; error?: string }

export class Store {
  scene: SceneDoc = defaultScene();
  revision = 0;                 // bumps on every scene change
  selection: Selection = { kind: 'none' };
  analysis: Analysis | null = null;
  time = 0;                     // playhead, seconds
  playing = false;
  loop = false;
  livePose: number[] | null = null;  // pose from the audio engine while playing
  hostDriven = false;           // plugin: the host's playhead drives time and pose
  hostActive = true;            // plugin: the host is running the plug-in (livePose is current)
  meters: number[] = [];        // per-layer dBFS
  filePath: string | null = null;
  dirty = false;
  audioInfo = new Map<string, AudioInfo>();

  private undoStack: string[] = [];
  private redoStack: string[] = [];
  private coalesceKey: string | null = null;
  private listeners: Listener[] = [];
  private pending = new Set<ChangeKind>();
  private scheduled = false;

  subscribe(fn: Listener): () => void {
    this.listeners.push(fn);
    return () => { this.listeners = this.listeners.filter((l) => l !== fn); };
  }

  emit(kind: ChangeKind): void {
    this.pending.add(kind);
    if (this.scheduled) return;
    this.scheduled = true;
    queueMicrotask(() => {
      this.scheduled = false;
      const kinds = this.pending;
      this.pending = new Set();
      for (const l of this.listeners) l(kinds);
    });
  }

  // Mutates the scene in place. Edits with the same `coalesce` key in a row
  // (one drag, one slider movement) make a single undo step.
  update(fn: (s: SceneDoc) => void, coalesce?: string): void {
    if (!coalesce || coalesce !== this.coalesceKey) {
      this.undoStack.push(JSON.stringify(this.scene));
      if (this.undoStack.length > 300) this.undoStack.shift();
      this.redoStack = [];
    }
    this.coalesceKey = coalesce ?? null;
    fn(this.scene);
    this.revision++;
    this.dirty = true;
    this.validateSelection();
    this.emit('scene');
  }

  // Ends a coalesced edit, so the next edit with the same key is a new undo step.
  endGesture(): void { this.coalesceKey = null; }

  undo(): void {
    const prev = this.undoStack.pop();
    if (!prev) return;
    this.redoStack.push(JSON.stringify(this.scene));
    this.scene = JSON.parse(prev);
    this.afterHistory();
  }

  redo(): void {
    const next = this.redoStack.pop();
    if (!next) return;
    this.undoStack.push(JSON.stringify(this.scene));
    this.scene = JSON.parse(next);
    this.afterHistory();
  }

  get canUndo() { return this.undoStack.length > 0; }
  get canRedo() { return this.redoStack.length > 0; }

  private afterHistory(): void {
    this.coalesceKey = null;
    this.revision++;
    this.dirty = true;
    this.validateSelection();
    this.emit('scene');
  }

  load(scene: SceneDoc, path: string | null): void {
    this.scene = completeScene(scene);
    this.filePath = path;
    this.undoStack = [];
    this.redoStack = [];
    this.coalesceKey = null;
    this.selection = { kind: 'none' };
    this.revision++;
    this.dirty = false;
    this.time = 0;
    this.emit('scene');
    this.emit('file');
    this.emit('selection');
  }

  // Takes a document the native side changed (plugin: tracks added or
  // renamed) without losing undo history, the file or the playhead.
  replace(scene: SceneDoc): void {
    if (JSON.stringify(scene) === JSON.stringify(this.scene)) return;
    this.undoStack.push(JSON.stringify(this.scene));
    if (this.undoStack.length > 300) this.undoStack.shift();
    this.coalesceKey = null;
    this.scene = completeScene(scene);
    this.revision++;
    this.validateSelection();
    this.emit('scene');
  }

  select(sel: Selection): void {
    this.selection = sel;
    this.emit('selection');
  }

  setTime(t: number): void {
    this.time = Math.max(0, t);
    this.emit('time');
  }

  setAnalysis(a: Analysis): void {
    this.analysis = a;
    this.emit('analysis');
  }

  // Effective timeline length: the scene's duration, or the longest
  // non-looping audio, or the time the listener needs to finish the path.
  get duration(): number {
    if (this.scene.duration > 0) return this.scene.duration;
    let d = 0;
    for (const l of this.scene.layers) {
      const info = this.audioInfo.get(l.audio);
      if (info && !l.loop) d = Math.max(d, l.start_time + info.duration);
    }
    if (this.analysis && this.analysis.arrival_time > 0) d = Math.max(d, this.analysis.arrival_time + 2);
    return Math.max(d, 30);
  }

  private validateSelection(): void {
    const s = this.selection;
    if (s.kind === 'layer' && s.index >= this.scene.layers.length) this.selection = { kind: 'none' };
    if (s.kind === 'point') {
      const p = pathOf(this.scene, s.path);
      if (!p || !p.segments[s.ref.seg] || !p.segments[s.ref.seg].points[s.ref.pt]) this.selection = { kind: 'none' };
    }
  }

  // Where layer i is drawn at time t: [x, y, z, yaw] of its centre, from the
  // engine's analysis (which covers layers with a path and layers carried
  // by a link); its own position when the analysis does not cover it.
  layerPlace(i: number, t: number): [number, number, number, number] {
    const l = this.scene.layers[i];
    const own: [number, number, number, number] = [l.position[0], l.position[1], l.position[2], 0];
    const tr = this.analysis?.layers?.find((x) => x.layer === i);
    if (!tr || !tr.samples.length) return own;
    const f = t / tr.dt;
    const a = Math.max(0, Math.min(tr.samples.length - 1, Math.floor(f)));
    const b = Math.min(tr.samples.length - 1, a + 1);
    const u = Math.max(0, Math.min(1, f - a));
    const p = tr.samples[a], q = tr.samples[b];
    // The analysis was taken with the layer where it stood then; a drag since
    // moves the whole journey with it.
    const s0 = tr.points[0];
    const d = s0 ? [l.position[0] - s0[0], l.position[1] - s0[1], l.position[2] - s0[2]] : [0, 0, 0];
    // A jump (loop restart) is not interpolated across.
    const jump = Math.hypot(q[0] - p[0], q[1] - p[1], q[2] - p[2]) > 2;
    const w = jump ? (u < 0.5 ? 0 : 1) : u;
    let dy = q[3] - p[3];
    if (dy > 180) dy -= 360;
    if (dy < -180) dy += 360;
    return [p[0] + (q[0] - p[0]) * w + d[0], p[1] + (q[1] - p[1]) * w + d[1], p[2] + (q[2] - p[2]) * w + d[2], p[3] + dy * w];
  }

  // Pose at the playhead: the audio engine's while playing (always, when the
  // host drives it and runs the plug-in: its automation is not in the
  // analysis), else the analysis. While Logic stands still it does not run
  // the plug-in, so the engine's pose would not follow edits.
  poseAt(t: number): number[] | null {
    if ((this.playing || (this.hostDriven && this.hostActive)) && this.livePose) return this.livePose;
    const a = this.analysis;
    if (!a || !a.poses.length) return null;
    const f = t / a.dt;
    const i = Math.max(0, Math.min(a.poses.length - 1, Math.floor(f)));
    const j = Math.min(a.poses.length - 1, i + 1);
    const u = Math.max(0, Math.min(1, f - i));
    const p = a.poses[i], q = a.poses[j];
    const out = p.map((v, k) => v + (q[k] - v) * u);
    // Angles: interpolate the short way round.
    for (const k of [3, 4, 5]) {
      let d = q[k] - p[k];
      if (d > 180) d -= 360;
      if (d < -180) d += 360;
      out[k] = p[k] + d * u;
    }
    return out;
  }
}

export const store = new Store();
