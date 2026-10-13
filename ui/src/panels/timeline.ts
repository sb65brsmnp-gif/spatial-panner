// Timeline under the viewport: transport, playhead/scrubbing, and keyframe
// lanes for the listener's speed along the path and head yaw and pitch.
//
// Keys: drag to move, double-click a lane to add, Delete to remove, the key
// bar edits the selected key's exact values and easing.
//
// With a layer selected the lanes are the layer's: its speed along its own
// path (or Path %, when it is timed by keys) with its own start and end
// lines, and its level (fades).
//
// The green "path start" line (when the listener sets off) and the red "path
// end" line (when it arrives) drag: the start takes the walk with it, the end
// stretches the speed curve. Cmd-drag over a time range selects the start,
// end and keys inside it; dragging the range moves them together.
//
// Transport: beginning of path (Enter), back and forward (a click jumps 5 s,
// holding scrubs at 4x), stop, play/pause (Space), end of path; , and . step
// the playhead 1 s (0.1 s with Shift).
import type { Analysis, Store } from '../model/store';
import { EASINGS, headKeyAt, sortKeys, speedAt, pathKeyAt, levelAt, hasPath, isLinked, linkedAt, SILENT_DB, type Easing, type HeadKey, type LayerDoc, type SceneDoc, type SpeedKey } from '../model/scene';
import { layerArrival, layerOfPath, layerPathLength, stretchSpeed } from '../model/layerMotion';
import type { Backend } from '../bridge/backend';
import { el, fmtTime, numberInput, select } from './dom';

type LaneId = 'speed' | 'yaw' | 'pitch' | 'lmove' | 'llevel' | 'llink';
interface AnyKey { time: number; easing?: Easing; level_db?: number; fraction?: number; linked?: boolean }
// How a lane's keys are read and written: absolute time and shown value.
interface KeyAccess {
  keys: AnyKey[];
  abs(k: AnyKey): number;
  setAbs(k: AnyKey, t: number): void;
  val(k: AnyKey): number;
  setVal(k: AnyKey, v: number): void;
  make(t: number, v: number): AnyKey;
  def: number;             // Option-click puts the value back to this
  curve(t: number): number;
}
// The walk a start and end line belong to: the listener's, or the selected
// layer's (speed timing).
interface Walk { start: number; end: number | null; setStart(s: SceneDoc, t: number): void; stretch(s: SceneDoc, from: number, to: number): void }
const LEVEL_MIN = -60;  // the bottom of the level lane: silence
interface Lane { id: LaneId; label: string; y: number; h: number; min: number; max: number; unit: string; marks?: [string, string] }
interface KeySel { lane: LaneId; index: number }

const GUTTER = 96;
const RULER = 22;

export const STEP = 1;            // , and . (seconds)
export const FINE_STEP = 0.1;     // with Shift
export const JUMP = 5;            // a click on back / forward
export const SCRUB_RATE = 4;      // holding back / forward: playhead seconds per real second
export const HOLD_MS = 250;       // holding longer than this scrubs instead of jumping

export type TransportKeyAction = { kind: 'start' } | { kind: 'step'; seconds: number };

// What a key does to the transport: Enter returns to the start of the path,
// , and . step back and forward (Shift: a fine step; on US layouts Shift
// turns them into < and >). Null for any other key.
export function transportKeyAction(e: { key: string; shiftKey: boolean }): TransportKeyAction | null {
  if (e.key === 'Enter') return { kind: 'start' };
  const step = e.shiftKey ? FINE_STEP : STEP;
  if (e.key === ',' || e.key === '<') return { kind: 'step', seconds: -step };
  if (e.key === '.' || e.key === '>') return { kind: 'step', seconds: step };
  return null;
}

// When the listener starts along the path (0 when unset).
export function pathStartTime(scene: SceneDoc): number {
  const t = scene.listener.path_start_time;
  return Number.isFinite(t) && t > 0 ? t : 0;
}

// Speed keys count from the start of the path (the engine: Pose.cpp,
// distanceAlongPath), so a key at 0 is the speed the listener sets off at,
// whenever that is. The timeline shows absolute time; these convert.
export function speedKeyTime(scene: SceneDoc, absolute: number): number {
  return Math.max(0, Math.round((absolute - pathStartTime(scene)) * 20) / 20);
}
export function speedKeyAbsolute(scene: SceneDoc, keyTime: number): number {
  return pathStartTime(scene) + keyTime;
}

// When the listener reaches the end of the path, as analysed by the engine;
// the end of the scene until the analysis says.
export function pathEndTime(analysis: Analysis | null, duration: number): number {
  const t = analysis && analysis.arrival_time > 0 ? analysis.arrival_time : duration;
  return Math.max(0, Math.min(duration, t));
}

export const MIN_WALK = 0.1;  // seconds: the end of the path never comes closer than this to its start

// Stretches the speed curve (whose keys count from the path start) so the
// walk that now ends at `oldEnd` ends at `newEnd` instead: key times scale
// by the ratio and speeds by its inverse, so each key keeps its place in the
// walk and the distance covered stays the path's length.
export function stretchPathEnd(scene: SceneDoc, oldEnd: number, newEnd: number): void {
  const start = pathStartTime(scene);
  newEnd = Math.max(start + MIN_WALK, newEnd);
  const f = (newEnd - start) / (oldEnd - start);
  if (!(f > 0) || !Number.isFinite(f) || Math.abs(f - 1) < 1e-9) return;
  for (const k of scene.listener.speed) {
    k.time = Math.round(k.time * f * 1000) / 1000;
    k.speed = Math.round((k.speed / f) * 10000) / 10000;
  }
}

// What a Cmd-drag over a time range takes hold of: the path start and end
// inside it, and the speed and head keys (by index) inside it.
export interface TimeSelection { t0: number; t1: number; start: boolean; end: boolean; speed: number[]; head: number[] }

export function selectRange(scene: SceneDoc, end: number | null, t0: number, t1: number): TimeSelection {
  const L = scene.listener;
  const inside = (t: number) => t >= t0 - 1e-9 && t <= t1 + 1e-9;
  const walk = L.paths.length > 0 && L.position_mode === 'speed';
  return {
    t0, t1,
    start: walk && inside(pathStartTime(scene)),
    end: walk && end !== null && inside(end),
    speed: walk ? L.speed.map((k, i) => (inside(speedKeyAbsolute(scene, k.time)) ? i : -1)).filter((i) => i >= 0) : [],
    head: L.head.keys.map((k, i) => (inside(k.time) ? i : -1)).filter((i) => i >= 0),
  };
}

// Moves everything in the selection by `dt` seconds (less, if something
// would go before 0). The start takes the speed curve and the end with it;
// the end on its own stretches the speed curve to the new end (its keys move
// with it); speed keys move by themselves only when neither is selected.
// Returns the shift applied; the selection's indices follow their keys.
export function shiftSelection(scene: SceneDoc, sel: TimeSelection, dt: number, end: number | null): number {
  const L = scene.listener;
  const start = pathStartTime(scene);
  const ownSpeed = !sel.start && !sel.end;
  let lo = -Infinity;
  if (sel.start) lo = Math.max(lo, -start);
  for (const i of sel.head) lo = Math.max(lo, -L.head.keys[i].time);
  if (ownSpeed) for (const i of sel.speed) lo = Math.max(lo, -L.speed[i].time);
  if (!sel.start && sel.end && end !== null) lo = Math.max(lo, start + MIN_WALK - end);
  dt = Math.max(dt, lo);
  if (!Number.isFinite(dt) || dt === 0) return 0;
  const r = (v: number) => Math.round(v * 1000) / 1000;
  if (sel.start) L.path_start_time = r(start + dt);
  const heads = sel.head.map((i) => L.head.keys[i]);
  for (const k of heads) k.time = r(k.time + dt);
  sortKeys(L.head.keys);
  sel.head = heads.map((k) => L.head.keys.indexOf(k));
  if (ownSpeed) {
    const speeds = sel.speed.map((i) => L.speed[i]);
    for (const k of speeds) k.time = r(k.time + dt);
    sortKeys(L.speed);
    sel.speed = speeds.map((k) => L.speed.indexOf(k));
  } else if (!sel.start && sel.end && end !== null) {
    stretchPathEnd(scene, end, end + dt);
  }
  sel.t0 += dt;
  sel.t1 += dt;
  return dt;
}

// The playhead moved by `seconds`, kept inside the scene and free of
// floating-point dust (ten 0.1 s steps land on a whole second).
export function steppedTime(time: number, seconds: number, duration: number): number {
  const t = Math.round((time + seconds) * 1000) / 1000;
  return Math.max(0, Math.min(duration, t));
}

export class Timeline {
  readonly root: HTMLElement;
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private t0 = 0;
  private t1 = 30;
  private userZoomed = false;
  private selKey: KeySel | null = null;
  private lanesOf = -1;  // whose lanes are shown: a layer's index, or -1 for the listener
  // A Cmd-dragged time range: the start, end and keys inside it move together.
  private range: TimeSelection | null = null;
  private drag:
    | { kind: 'scrub' }
    | { kind: 'key'; sel: KeySel }
    // The path start or end line. `end`: the end as last placed (the analysis
    // that normally gives it lags behind the drag).
    | { kind: 'marker'; which: 'start' | 'end'; end: number }
    | { kind: 'range'; anchor: number }                     // Cmd-drag: choosing the range
    | { kind: 'group'; last: number; end: number | null }  // moving the range
    | null = null;
  private keyBar: HTMLElement;
  private timeLabel: HTMLElement;
  private idleLabel: HTMLElement;
  private playBtn: HTMLButtonElement;
  private loopBtn: HTMLButtonElement;
  private durationInput: HTMLInputElement;
  private lastSeek = 0;
  private focused = false;
  // Back / forward button held down: a timer until the hold becomes a scrub,
  // then the scrub itself (direction, whether to resume playing on release).
  private hold: { timer: number; dir: -1 | 1; scrub: { wasPlaying: boolean; last: number; frame: number } | null } | null = null;

  constructor(private store: Store, private backend: Backend) {
    this.root = el('div', { id: 'timeline' });
    const bar = el('div', { class: 'tl-bar' });
    const btn = (label: string, title: string, fn: () => void) => {
      const b = el('button', { class: 'tbtn', title }, label) as HTMLButtonElement;
      b.addEventListener('click', fn);
      return b;
    };
    const startBtn = btn('❚◀', 'Beginning of path (Enter)', () => this.goToPathStart());
    const backBtn = this.holdButton('◀◀', `Back ${JUMP} s; hold to scrub back. Step back: , (${STEP} s), Shift+, (${FINE_STEP} s)`, -1);
    const stopBtn = btn('■', 'Stop and return to start', () => this.transport('stop'));
    this.playBtn = btn('▶', 'Play / pause (Space)', () => this.togglePlay());
    const fwdBtn = this.holdButton('▶▶', `Forward ${JUMP} s; hold to scrub forward. Step forward: . (${STEP} s), Shift+. (${FINE_STEP} s)`, 1);
    const endBtn = btn('▶❚', 'End of path', () => this.goToPathEnd());
    this.loopBtn = btn('⟲', 'Loop', () => {
      this.store.loop = !this.store.loop;
      this.backend.transport({ action: 'loop', loop: this.store.loop });
      this.refreshBar();
    });
    this.timeLabel = el('span', { class: 'tl-time' }, '0:00.0');
    // Logic stands still: it does not run the plug-in then, so nothing here
    // can follow its playhead until it plays (or the track is armed).
    this.idleLabel = el('span', { class: 'tl-idle', title: 'Logic runs a plug-in only while playing, or on a track that is record-enabled or input-monitored. Until then the playhead and listener here stay where Logic last ran it.' },
      'Logic is idle: the playhead follows once Logic plays (or with this track record-enabled)');
    this.idleLabel.style.display = 'none';
    this.durationInput = numberInput(0, (v) => this.store.update((s) => { s.duration = Math.max(0, v); }), { step: 1, min: 0, width: 56, def: 0 });
    this.durationInput.title = 'Scene length in seconds (0 = automatic: longest non-looping audio, or the path)';
    this.keyBar = el('div', { class: 'tl-keybar' });
    const transport = [startBtn, backBtn, stopBtn, this.playBtn, fwdBtn, endBtn, this.loopBtn];
    for (const b of transport) b.classList.add('transport');
    // Plugin: Logic owns the transport; the playhead here follows Logic's.
    if (backend.kind === 'plugin') for (const b of transport) b.style.display = 'none';
    bar.append(...transport, this.timeLabel, el('span', { class: 'tl-sep' }, 'Length'), this.durationInput,
      el('span', { class: 'tl-unit' }, 's'), this.idleLabel, this.keyBar);
    this.canvas = el('canvas', { class: 'tl-canvas' }) as HTMLCanvasElement;
    this.ctx = this.canvas.getContext('2d')!;
    this.root.append(bar, this.canvas);

    new ResizeObserver(() => this.draw()).observe(this.canvas);
    this.canvas.addEventListener('pointerdown', (e) => this.down(e));
    this.canvas.addEventListener('pointermove', (e) => this.hover(e));
    window.addEventListener('pointermove', (e) => this.move(e));
    window.addEventListener('pointerup', () => this.up());
    this.canvas.addEventListener('dblclick', (e) => this.dblclick(e));
    this.canvas.addEventListener('wheel', (e) => this.wheel(e), { passive: false });
    window.addEventListener('pointerdown', (e) => { this.focused = this.root.contains(e.target as Node); }, { capture: true });
    window.addEventListener('keydown', (e) => this.key(e));
    store.subscribe(() => {
      // Another layer (or the listener) selected: its lanes, nothing selected in them.
      const li = this.layerIndex();
      if (li !== this.lanesOf) { this.lanesOf = li; this.selKey = null; this.range = null; }
      this.refreshBar();
      this.draw();
    });
    this.refreshBar();
  }

  // ------------------------------------------------------------ layout

  // The selected layer (its point selected counts), or -1.
  private layerIndex(): number {
    const sel = this.store.selection;
    const i = sel.kind === 'layer' ? sel.index : sel.kind === 'point' ? layerOfPath(sel.path) : -1;
    return this.store.scene.layers[i] ? i : -1;
  }
  private get layer(): LayerDoc | null { const i = this.layerIndex(); return i >= 0 ? this.store.scene.layers[i] : null; }

  private lanes(): Lane[] {
    const h = this.canvas.clientHeight - RULER;
    const l = this.layer;
    if (l) {
      // A linked layer gets a third, step lane: linked (1) or not (0).
      const linked = isLinked(l);
      const kh = linked ? Math.max(26, Math.floor(h / 4)) : 0;
      const lh = Math.max(30, Math.floor((h - kh) / 2));
      const m = l.motion;
      const move: Lane = m?.timing === 'keys'
        ? { id: 'lmove', label: 'Path %', y: RULER, h: lh, min: 0, max: 100, unit: '%' }
        : { id: 'lmove', label: 'Layer speed', y: RULER, h: lh, min: 0, max: Math.ceil(Math.max(3, ...(m?.speed ?? []).map((k) => k.speed * 1.2))), unit: 'm/s' };
      const lanes: Lane[] = [move, { id: 'llevel', label: 'Layer level', y: RULER + lh, h: h - lh - kh, min: LEVEL_MIN, max: 12, unit: 'dB' }];
      if (linked) lanes.push({ id: 'llink', label: 'Link', y: RULER + (h - kh), h: kh, min: 0, max: 1, unit: '', marks: ['free', 'linked'] });
      return lanes;
    }
    const lh = Math.max(30, Math.floor(h / 3));
    const keys = this.store.scene.listener.speed;
    const maxSpeed = Math.max(3, ...keys.map((k) => k.speed * 1.2));
    const rel = this.store.scene.listener.head.mode !== 'keyframed';
    return [
      { id: 'speed', label: 'Speed', y: RULER, h: lh, min: 0, max: Math.ceil(maxSpeed), unit: 'm/s' },
      { id: 'yaw', label: rel ? 'Head yaw ±' : 'Head yaw', y: RULER + lh, h: lh, min: -180, max: 180, unit: '°' },
      { id: 'pitch', label: rel ? 'Head pitch ±' : 'Head pitch', y: RULER + 2 * lh, h: h - 2 * lh, min: -90, max: 90, unit: '°' },
    ];
  }

  private x(t: number): number {
    const w = this.canvas.clientWidth - GUTTER;
    return GUTTER + ((t - this.t0) / (this.t1 - this.t0)) * w;
  }
  private t(x: number): number {
    const w = this.canvas.clientWidth - GUTTER;
    return this.t0 + ((x - GUTTER) / w) * (this.t1 - this.t0);
  }
  private y(l: Lane, v: number): number { return l.y + l.h - 4 - ((v - l.min) / (l.max - l.min)) * (l.h - 8); }
  private v(l: Lane, y: number): number { return l.min + ((l.y + l.h - 4 - y) / (l.h - 8)) * (l.max - l.min); }

  private laneAt(y: number): Lane | null {
    return this.lanes().find((l) => y >= l.y && y < l.y + l.h) ?? null;
  }

  private keysFor(lane: LaneId): { t: number; v: number }[] {
    const a = this.access(lane);
    return a ? a.keys.map((k) => ({ t: a.abs(k), v: a.val(k) })) : [];
  }

  // Reading and writing a lane's keys (null: the lane has none to edit).
  private access(lane: LaneId, scene: SceneDoc = this.store.scene): KeyAccess | null {
    const L = scene.listener;
    const r = (v: number, q: number) => Math.round(v * q) / q;
    if (lane === 'speed') {
      if (L.position_mode === 'along_path') return null;
      return { keys: L.speed, abs: (k) => speedKeyAbsolute(scene, k.time), setAbs: (k, t) => { k.time = speedKeyTime(scene, t); },
        val: (k) => (k as SpeedKey).speed, setVal: (k, v) => { (k as SpeedKey).speed = Math.max(0, r(v, 20)); },
        make: (t, v) => ({ time: speedKeyTime(scene, t), speed: Math.max(0, r(v, 20)), easing: L.speed.length ? L.speed[0].easing : 'linear' }) as SpeedKey,
        def: 1.4, curve: (t) => speedAt(L.speed, t - pathStartTime(scene)) };
    }
    if (lane === 'yaw' || lane === 'pitch') {
      const f = lane;
      return { keys: L.head.keys, abs: (k) => k.time, setAbs: (k, t) => { k.time = t; },
        val: (k) => (k as HeadKey)[f], setVal: (k, v) => { (k as HeadKey)[f] = Math.round(v); },
        make: (t, v) => { const c = headKeyAt(L.head.keys, t); return { time: t, yaw: f === 'yaw' ? Math.round(v) : Math.round(c.yaw), pitch: f === 'pitch' ? Math.round(v) : Math.round(c.pitch), roll: c.roll, easing: 'smooth' } as HeadKey; },
        def: 0, curve: (t) => { if (!L.head.keys.length) return 0; const h = headKeyAt(L.head.keys, t); return h[f]; } };
    }
    const l = scene.layers[this.layerIndex()];
    if (!l) return null;
    if (lane === 'llink') {
      if (!isLinked(l)) return null;
      const link = l.link!;
      return { keys: link.keys, abs: (k) => k.time, setAbs: (k, t) => { k.time = t; },
        val: (k) => (k.linked ? 1 : 0), setVal: (k, v) => { k.linked = v >= 0.5; },
        make: (t, v) => ({ time: t, linked: v >= 0.5 }) as AnyKey,
        def: 1, curve: (t) => (linkedAt(link, t) ? 1 : 0) };
    }
    if (lane === 'llevel') {
      const keys = l.level_keys ?? [];
      const lv = (v: number) => (v <= LEVEL_MIN + 0.5 ? SILENT_DB : Math.min(12, r(v, 2)));
      return { keys, abs: (k) => k.time, setAbs: (k, t) => { k.time = t; },
        val: (k) => Math.max(LEVEL_MIN, (k as { level_db: number }).level_db), setVal: (k, v) => { (k as { level_db: number }).level_db = lv(v); },
        make: (t, v) => ({ time: t, level_db: lv(v), easing: 'linear' }) as AnyKey,
        def: 0, curve: (t) => (keys.length ? Math.max(LEVEL_MIN, levelAt(keys as never, t)) : 0) };
    }
    const m = l.motion;
    if (!m || !hasPath(l) || m.timing === 'position') return null;
    if (m.timing === 'keys') {
      return { keys: m.keys, abs: (k) => k.time, setAbs: (k, t) => { k.time = t; },
        val: (k) => (k as { fraction: number }).fraction * 100, setVal: (k, v) => { (k as { fraction: number }).fraction = Math.max(0, Math.min(1, r(v, 10) / 100)); },
        make: (t, v) => ({ time: t, fraction: Math.max(0, Math.min(1, r(v, 10) / 100)), easing: 'smooth' }) as AnyKey,
        def: 0, curve: (t) => (m.keys.length ? pathKeyAt(m.keys, t) * 100 : 0) };
    }
    const rel = (t: number) => Math.max(0, r(t - m.start_time, 20));
    return { keys: m.speed, abs: (k) => m.start_time + k.time, setAbs: (k, t) => { k.time = rel(t); },
      val: (k) => (k as SpeedKey).speed, setVal: (k, v) => { (k as SpeedKey).speed = Math.max(0, r(v, 20)); },
      make: (t, v) => ({ time: rel(t), speed: Math.max(0, r(v, 20)), easing: m.speed.length ? m.speed[0].easing : 'linear' }) as SpeedKey,
      def: 1.4, curve: (t) => (t < m.start_time ? 0 : speedAt(m.speed, t - m.start_time)) };
  }

  // The start and end lines: the selected layer's (speed timing), else the
  // listener's walk.
  private walk(): Walk | null {
    const l = this.layer;
    if (l) {
      const m = l.motion;
      if (!m || !hasPath(l) || m.timing !== 'speed') return null;
      const i = this.layerIndex();
      const length = layerPathLength(this.store.scene, this.store.analysis, i);
      return { start: m.start_time, end: layerArrival(l, length),
        setStart: (s, t) => { s.layers[i].motion!.start_time = t; },
        stretch: (s, from, to) => { const mm = s.layers[i].motion!; stretchSpeed(mm.speed, (to - mm.start_time) / (from - mm.start_time)); } };
    }
    if (!this.hasWalk()) return null;
    const a = this.store.analysis;
    return { start: pathStartTime(this.store.scene), end: a && a.arrival_time > 0 ? a.arrival_time : null,
      setStart: (s, t) => { s.listener.path_start_time = t; }, stretch: (s, from, to) => stretchPathEnd(s, from, to) };
  }

  // ------------------------------------------------------------ drawing

  draw(): void {
    const c = this.canvas, ctx = this.ctx;
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    if (!w || !h) return;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    if (!this.userZoomed) { this.t0 = 0; this.t1 = this.store.duration; }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = '#181b21';
    ctx.fillRect(0, 0, w, h);
    ctx.font = '11px system-ui, sans-serif';

    // Ruler.
    ctx.fillStyle = '#20242c';
    ctx.fillRect(GUTTER, 0, w - GUTTER, RULER);
    const span = this.t1 - this.t0;
    const step = niceStep(span / Math.max(1, (w - GUTTER) / 70));
    ctx.fillStyle = '#9aa3b2';
    ctx.strokeStyle = '#2c313b';
    for (let t = Math.ceil(this.t0 / step) * step; t <= this.t1; t += step) {
      const x = this.x(t);
      ctx.fillText(fmtTime(t, step < 1), x + 3, 14);
      ctx.beginPath(); ctx.moveTo(x + 0.5, RULER - 6); ctx.lineTo(x + 0.5, h); ctx.stroke();
    }
    // Scene end.
    const dur = this.store.duration;
    if (dur < this.t1) {
      ctx.fillStyle = 'rgba(0,0,0,0.35)';
      ctx.fillRect(this.x(dur), RULER, w - this.x(dur), h - RULER);
    }

    const L = this.store.scene.listener;
    for (const lane of this.lanes()) {
      ctx.strokeStyle = '#2c313b';
      ctx.beginPath(); ctx.moveTo(0, lane.y + 0.5); ctx.lineTo(w, lane.y + 0.5); ctx.stroke();
      ctx.fillStyle = '#c9d1dc';
      ctx.fillText(lane.label, 8, lane.y + 16);
      ctx.fillStyle = '#6f7889';
      if (lane.marks) {
        ctx.fillText(lane.marks[1], 8, lane.y + lane.h - 6 - (lane.h - 12) * 0.5 + 4);
      } else {
        ctx.fillText(`${lane.max}${lane.unit}`, 8, lane.y + 30);
        ctx.fillText(`${lane.min}${lane.unit}`, 8, lane.y + lane.h - 6);
      }
      if (lane.id !== 'speed' && lane.id !== 'llink') {
        const zy = this.y(lane, 0);
        ctx.strokeStyle = '#323844';
        ctx.setLineDash([3, 3]);
        ctx.beginPath(); ctx.moveTo(GUTTER, zy); ctx.lineTo(w, zy); ctx.stroke();
        ctx.setLineDash([]);
      }
      ctx.save();
      ctx.beginPath(); ctx.rect(GUTTER, lane.y, w - GUTTER, lane.h); ctx.clip();
      this.drawLaneCurves(lane);
      ctx.restore();
    }

    const layer = this.layer;
    const cover = layer ? (!hasPath(layer) ? `${layer.name || 'This layer'} has no path: draw one in the Layers tab to move it.`
      : layer.motion!.timing === 'position' ? 'This layer stays at a point along its path (Layers tab, "Position").' : null)
      : L.position_mode === 'along_path' ? 'Position is set by "Position along path" (Path tab), not by speed.' : null;
    if (cover) {
      const lane = this.lanes()[0];
      ctx.fillStyle = 'rgba(24,27,33,0.8)';
      ctx.fillRect(GUTTER, lane.y + 1, w - GUTTER, lane.h - 1);
      ctx.fillStyle = '#9aa3b2';
      ctx.fillText(cover, GUTTER + 10, lane.y + lane.h / 2);
    }
    // Whose lanes these are.
    if (layer) {
      ctx.fillStyle = layer.color ?? '#4f9cf9';
      ctx.fillText((layer.name || `Layer ${this.layerIndex() + 1}`).slice(0, 14), 8, 14);
    }

    // The Cmd-dragged range: what is inside moves together.
    const range = this.range;
    if (range) {
      const x0 = this.x(range.t0), x1 = this.x(range.t1);
      ctx.fillStyle = 'rgba(86, 204, 242, 0.14)';
      ctx.fillRect(x0, RULER, Math.max(1, x1 - x0), h - RULER);
      ctx.strokeStyle = 'rgba(86, 204, 242, 0.7)';
      ctx.beginPath(); ctx.moveTo(x0 + 0.5, RULER); ctx.lineTo(x0 + 0.5, h); ctx.moveTo(x1 + 0.5, RULER); ctx.lineTo(x1 + 0.5, h); ctx.stroke();
    }

    // The listener waits at the start of the path until path_start_time:
    // the green line (drag it to move the walk, speed keys and end with it).
    const walk = this.walk();
    if (walk) {
      const start = walk.start;
      const lane = this.lanes()[0];
      const sx = Math.min(w, this.x(start));
      if (sx > GUTTER) {
        ctx.fillStyle = 'rgba(24,27,33,0.6)';
        ctx.fillRect(GUTTER, lane.y + 1, sx - GUTTER, lane.h - 1);
      }
      this.marker(sx, '#5fd38d', layer ? 'layer sets off' : 'path start', !!range?.start);
    }

    // Arrival at the end of the path: the red line (drag it to make the walk
    // faster or slower).
    const end = this.endMarker();
    if (end !== null) this.marker(this.x(end), '#eb5757', layer ? 'layer arrives' : 'path end', !!range?.end);


    // Playhead.
    const px = this.x(this.store.time);
    ctx.strokeStyle = '#ffffff';
    ctx.beginPath(); ctx.moveTo(px + 0.5, 0); ctx.lineTo(px + 0.5, h); ctx.stroke();
    ctx.fillStyle = '#ffffff';
    ctx.beginPath(); ctx.moveTo(px - 5, 0); ctx.lineTo(px + 6, 0); ctx.lineTo(px + 0.5, 8); ctx.fill();
  }

  // A vertical line with its label, wider when selected.
  private marker(x: number, color: string, label: string, selected: boolean): void {
    const ctx = this.ctx;
    const h = this.canvas.clientHeight;
    ctx.strokeStyle = color;
    ctx.lineWidth = selected ? 3 : 1;
    ctx.beginPath(); ctx.moveTo(x + 0.5, RULER); ctx.lineTo(x + 0.5, h); ctx.stroke();
    ctx.lineWidth = 1;
    ctx.fillStyle = color;
    ctx.fillText(label, x + 4, RULER + 12);
  }

  // A path the listener walks along at the speed curve (the start and end
  // lines exist).
  private hasWalk(): boolean {
    const L = this.store.scene.listener;
    return L.paths.length > 0 && L.position_mode === 'speed';
  }

  // The end of the path as drawn: where the listener arrives (the analysis),
  // or where the end is being dragged to.
  private endMarker(): number | null {
    const d = this.drag;
    if (d?.kind === 'marker' && d.which === 'end') return d.end;
    if (d?.kind === 'group' && d.end !== null && this.range?.end) return d.end;
    return this.walk()?.end ?? null;
  }

  // The start or end line under `x`, if any.
  private markerAt(x: number): 'start' | 'end' | null {
    const walk = this.walk();
    if (!walk) return null;
    const end = this.endMarker();
    if (end !== null && Math.abs(this.x(end) - x) < 6) return 'end';
    if (Math.abs(this.x(walk.start) - x) < 6) return 'start';
    return null;
  }

  private drawLaneCurves(lane: Lane): void {
    const ctx = this.ctx;
    const w = this.canvas.clientWidth;
    const a = this.store.analysis;
    // Resulting head angle from the engine (faint), so relative keys can be
    // read against where the head actually points.
    if ((lane.id === 'yaw' || lane.id === 'pitch') && a && a.poses.length) {
      ctx.strokeStyle = 'rgba(255, 200, 87, 0.35)';
      ctx.beginPath();
      let prev: number | null = null;
      const k = lane.id === 'yaw' ? 3 : 4;
      for (let x = GUTTER; x <= w; x += 2) {
        const t = this.t(x);
        const i = Math.round(t / a.dt);
        if (i < 0 || i >= a.poses.length) continue;
        const v = a.poses[i][k];
        const y = this.y(lane, v);
        if (prev === null || Math.abs(v - prev) > 180) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        prev = v;
      }
      ctx.stroke();
    }
    // The key curve.
    const acc = this.access(lane.id);
    if (!acc) return;
    const head = lane.id === 'yaw' || lane.id === 'pitch';
    const color = lane.id === 'speed' || lane.id === 'lmove' ? '#5fd38d' : lane.id === 'yaw' ? '#56ccf2' : lane.id === 'llevel' ? '#f2c94c' : lane.id === 'llink' ? '#f2994a' : '#bb6bd9';
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let x = GUTTER; x <= w; x += 2) {
      const v = acc.curve(this.t(x));
      const y = this.y(lane, Math.max(lane.min, Math.min(lane.max, v)));
      if (x === GUTTER) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.lineWidth = 1;
    // Keys.
    this.keysFor(lane.id).forEach((k, i) => {
      const x = this.x(k.t), y = this.y(lane, Math.max(lane.min, Math.min(lane.max, k.v)));
      const sk = this.selKey;
      const sel = (sk && sk.index === i && (sk.lane === lane.id || (head && (sk.lane === 'yaw' || sk.lane === 'pitch'))))
        || (this.range && (lane.id === 'speed' ? this.range.speed : head ? this.range.head : []).includes(i));
      ctx.fillStyle = sel ? '#ffffff' : color;
      ctx.beginPath();
      ctx.moveTo(x, y - 5); ctx.lineTo(x + 5, y); ctx.lineTo(x, y + 5); ctx.lineTo(x - 5, y); ctx.closePath();
      ctx.fill();
    });
  }

  // ---------------------------------------------------------- the bar

  private refreshBar(): void {
    this.playBtn.textContent = this.store.playing ? '❚❚' : '▶';
    this.loopBtn.classList.toggle('on', this.store.loop);
    this.timeLabel.textContent = `${fmtTime(this.store.time, true)} / ${fmtTime(this.store.duration, false)}`;
    this.idleLabel.style.display = this.store.hostDriven && !this.store.hostActive ? '' : 'none';
    if (document.activeElement !== this.durationInput) this.durationInput.value = String(this.store.scene.duration);
    this.renderKeyBar();
  }

  private renderKeyBar(): void {
    const s = this.selKey;
    const acc = s ? this.access(s.lane) : null;
    const key = s && acc ? acc.keys[s.index] ?? null : null;
    const r = this.range;
    const sig = key ? JSON.stringify([s, key]) : r ? JSON.stringify(['range', r.start, r.end, r.speed.length, r.head.length]) : '';
    if (this.keyBar.dataset.sig === sig) return;
    this.keyBar.dataset.sig = sig;
    this.keyBar.replaceChildren();
    if (r && !key) {
      const parts: string[] = [];
      if (r.start) parts.push('path start');
      if (r.end) parts.push('path end');
      const n = r.speed.length + r.head.length;
      if (n) parts.push(`${n} key${n === 1 ? '' : 's'}`);
      this.keyBar.append(el('span', { class: 'hint' }, parts.length
        ? `${parts.join(', ')} selected: drag the band to move them together (Delete removes the keys, Esc deselects)`
        : 'Nothing in the range: Cmd-drag over the start, end and keys to move them together'));
      return;
    }
    if (!s || !key) {
      this.keyBar.append(el('span', { class: 'hint' }, this.layer
        ? `Lanes of the selected layer: double-click to add a key${this.walk() ? ', drag its start and end lines' : ''}; Esc deselects the layer to edit the listener's lanes`
        : this.hasWalk()
        ? 'Double-click a lane to add a key; drag the start and end lines; Cmd-drag a range to move everything in it'
        : 'Double-click a lane to add a key'));
      return;
    }
    const upd = (fn: () => void) => this.store.update(() => { fn(); this.resortSelected(); }, `key-edit-${s.lane}-${s.index}`);
    const fromStart = (s.lane === 'speed' && pathStartTime(this.store.scene) > 0)
      || (s.lane === 'lmove' && this.layer?.motion?.timing === 'speed' && this.layer.motion.start_time > 0);
    this.keyBar.append(el('span', { class: 'tl-sep' }, 'Key at'),
      numberInput(key.time, (v) => upd(() => { key.time = Math.max(0, v); }), { step: 0.1, width: 56 }),
      el('span', { class: 'tl-unit' }, fromStart ? (s.lane === 'lmove' ? 's after the layer sets off' : 's after the path starts') : 's'));
    if (s.lane === 'llink') {
      this.keyBar.append(select(['linked', 'free'], key.linked ? 'linked' : 'free', (v) => upd(() => { key.linked = v === 'linked'; }), { linked: 'links it', free: 'unlinks it' }, 'linked'));
      const del = el('button', { class: 'tbtn small', title: 'Delete key (Delete)' }, '✕');
      del.addEventListener('click', () => this.deleteKey());
      this.keyBar.append(del);
      return;
    }
    if (s.lane === 'lmove' || s.lane === 'llevel') {
      const a = acc!;
      const unit = s.lane === 'llevel' ? 'dB' : this.layer?.motion?.timing === 'keys' ? '% of the path' : 'm/s';
      const shown = s.lane === 'llevel' && (key.level_db ?? 0) <= SILENT_DB ? LEVEL_MIN : a.val(key);
      this.keyBar.append(numberInput(Math.round(shown * 100) / 100, (v) => upd(() => a.setVal(key, v)), { step: s.lane === 'llevel' ? 0.5 : 0.1, width: 56, def: a.def }),
        el('span', { class: 'tl-unit' }, s.lane === 'llevel' && shown <= LEVEL_MIN ? 'dB (silent)' : unit));
    } else if (s.lane === 'speed') {
      const k = key as SpeedKey;
      this.keyBar.append(numberInput(k.speed, (v) => upd(() => { k.speed = Math.max(0, v); }), { step: 0.1, width: 52, def: 1.4 }),
        el('span', { class: 'tl-unit' }, 'm/s'));
    } else {
      const k = key as HeadKey;
      this.keyBar.append(el('span', { class: 'tl-sep' }, 'Yaw'), numberInput(k.yaw, (v) => upd(() => { k.yaw = v; }), { step: 1, width: 52, def: 0 }),
        el('span', { class: 'tl-sep' }, 'Pitch'), numberInput(k.pitch, (v) => upd(() => { k.pitch = Math.max(-90, Math.min(90, v)); }), { step: 1, width: 52, def: 0 }));
    }
    this.keyBar.append(el('span', { class: 'tl-sep' }, 'then'),
      select(EASINGS, key.easing ?? 'linear', (v) => upd(() => { key.easing = v as Easing; }), { linear: 'linear', smooth: 'smooth', ease_in: 'ease in', ease_out: 'ease out', hold: 'hold' }, 'linear'));
    const del = el('button', { class: 'tbtn small', title: 'Delete key (Delete)' }, '✕');
    del.addEventListener('click', () => this.deleteKey());
    this.keyBar.append(del);
  }

  private resortSelected(): void {
    const s = this.selKey;
    if (!s) return;
    const arr = this.access(s.lane)?.keys;
    if (!arr) return;
    const obj = arr[s.index];
    sortKeys(arr);
    s.index = arr.indexOf(obj);
  }

  // ---------------------------------------------------------- input

  private togglePlay(): void {
    if (this.hostTransport()) return;
    this.transport(this.store.playing ? 'pause' : 'play');
  }

  // Plugin: Logic owns the transport; the playhead here follows Logic's.
  private lastHint = -Infinity;
  private hostTransport(): boolean {
    if (this.backend.kind !== 'plugin') return false;
    const now = performance.now();
    if (now - this.lastHint > 3000) {
      this.lastHint = now;
      window.dispatchEvent(new CustomEvent('sp-message', { detail: { text: 'Move the playhead and play in Logic; the editor follows it.', level: 'info' } }));
    }
    return true;
  }

  private transport(action: 'play' | 'pause' | 'stop'): void {
    if (action === 'play') this.backend.transport({ action: 'seek', time: this.store.time });
    this.backend.transport({ action, loop: this.store.loop });
    if (action === 'stop') this.store.setTime(0);
    this.store.playing = action === 'play';
    this.refreshBar();
  }

  // Where the listener starts along the path (Enter, the ❚◀ button).
  goToPathStart(): void { this.jumpTo(pathStartTime(this.store.scene)); }

  // Where the listener arrives at the end of the path (the ▶❚ button).
  goToPathEnd(): void { this.jumpTo(pathEndTime(this.store.analysis, this.store.duration)); }

  // Moves the playhead by `seconds` (, and . keys).
  nudge(seconds: number): void { this.jumpTo(steppedTime(this.store.time, seconds, this.store.duration)); }

  // A discrete jump: the engine always gets this seek (unlike a drag's, which
  // is rate-limited), and playing goes on from the new time.
  private jumpTo(t: number): void {
    if (this.hostTransport()) return;
    t = Math.max(0, Math.min(this.store.duration, t));
    this.store.setTime(t);
    this.lastSeek = performance.now();
    this.backend.transport({ action: 'seek', time: t });
  }

  private scrubTo(t: number): void {
    if (this.hostTransport()) return;
    t = Math.max(0, Math.min(this.store.duration, t));
    this.store.setTime(t);
    const now = performance.now();
    if (now - this.lastSeek > 40) {
      this.lastSeek = now;
      this.backend.transport({ action: 'seek', time: t });
    }
  }

  // Back / forward: a click jumps JUMP seconds; holding the button longer
  // than HOLD_MS scrubs the playhead at SCRUB_RATE times real time (audio
  // paused meanwhile) and, if it was playing, plays on from where it lands.
  private holdButton(label: string, title: string, dir: -1 | 1): HTMLButtonElement {
    const b = el('button', { class: 'tbtn', title }, label) as HTMLButtonElement;
    b.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || this.hold) return;
      e.preventDefault();
      b.setPointerCapture(e.pointerId);
      const timer = window.setTimeout(() => this.beginScrub(), HOLD_MS);
      this.hold = { timer, dir, scrub: null };
    });
    const release = () => this.releaseHold();
    b.addEventListener('pointerup', release);
    b.addEventListener('pointercancel', release);
    b.addEventListener('lostpointercapture', release);
    return b;
  }

  private beginScrub(): void {
    const h = this.hold;
    if (!h || h.scrub) return;
    const wasPlaying = this.store.playing;
    if (wasPlaying) this.transport('pause');
    h.scrub = { wasPlaying, last: performance.now(), frame: 0 };
    const frame = () => {
      const s = this.hold?.scrub;
      if (!s) return;
      const now = performance.now();
      this.scrubTo(this.store.time + h.dir * SCRUB_RATE * (now - s.last) / 1000);
      s.last = now;
      s.frame = requestAnimationFrame(frame);
    };
    frame();
  }

  private releaseHold(): void {
    const h = this.hold;
    if (!h) return;
    this.hold = null;
    window.clearTimeout(h.timer);
    if (!h.scrub) { this.jumpTo(this.store.time + h.dir * JUMP); return; }
    cancelAnimationFrame(h.scrub.frame);
    this.jumpTo(this.store.time);
    if (h.scrub.wasPlaying) this.transport('play');
  }

  private hitKey(x: number, y: number): KeySel | null {
    const lane = this.laneAt(y);
    if (!lane) return null;
    const keys = this.keysFor(lane.id);
    for (let i = keys.length - 1; i >= 0; i--) {
      const kx = this.x(keys[i].t), ky = this.y(lane, Math.max(lane.min, Math.min(lane.max, keys[i].v)));
      if (Math.abs(kx - x) < 7 && Math.abs(ky - y) < 7) return { lane: lane.id, index: i };
    }
    return null;
  }

  private local(e: MouseEvent): [number, number] {
    const r = this.canvas.getBoundingClientRect();
    return [e.clientX - r.left, e.clientY - r.top];
  }

  private snap(t: number): number { return Math.max(0, Math.round(t * 20) / 20); }

  private hover(e: PointerEvent): void {
    if (this.drag) return;
    const [x, y] = this.local(e);
    const t = this.t(x);
    const inRange = !!this.range && x >= GUTTER && t >= this.range.t0 && t <= this.range.t1;
    this.canvas.style.cursor = x < GUTTER ? '' : inRange ? 'grab' : (!this.hitKey(x, y) && this.markerAt(x)) ? 'col-resize' : '';
  }

  private down(e: PointerEvent): void {
    const [x, y] = this.local(e);
    if (x < GUTTER) return;
    const t = this.t(x);
    if ((e.metaKey || e.ctrlKey) && !this.layer) {
      // Cmd-drag: a time range; the start, end and keys inside it move together.
      this.selKey = null;
      this.range = selectRange(this.store.scene, this.endMarker(), t, t);
      this.drag = { kind: 'range', anchor: t };
      this.refreshBar();
      this.draw();
      return;
    }
    if (this.range && t >= this.range.t0 && t <= this.range.t1) {
      this.drag = { kind: 'group', last: this.snap(t), end: this.endMarker() };
      this.canvas.style.cursor = 'grabbing';
      return;
    }
    if (this.range) { this.range = null; this.refreshBar(); this.draw(); }
    const hit = this.hitKey(x, y);
    const marker = hit ? null : this.markerAt(x);
    if (marker) {
      this.selKey = null;
      this.drag = { kind: 'marker', which: marker, end: this.endMarker() ?? 0 };
      this.refreshBar();
      this.draw();
      return;
    }
    if (y < RULER) {
      this.drag = { kind: 'scrub' };
      this.scrubTo(t);
      return;
    }
    if (hit && this.access(hit.lane)) {
      this.selKey = hit;
      if (e.altKey) {
        // Option-click: the key's value back to its default (time stays).
        this.store.update((sc) => { const a = this.access(hit.lane, sc); if (a?.keys[hit.index]) a.setVal(a.keys[hit.index], a.def); });
        this.refreshBar();
        this.draw();
        return;
      }
      this.drag = { kind: 'key', sel: hit };
      this.refreshBar();
      this.draw();
      return;
    }
    this.selKey = null;
    this.drag = { kind: 'scrub' };
    this.scrubTo(this.t(x));
    this.refreshBar();
  }

  private move(e: PointerEvent): void {
    const d = this.drag;
    if (!d) return;
    const [x, y] = this.local(e);
    if (d.kind === 'scrub') { this.scrubTo(this.t(x)); return; }
    if (d.kind === 'range') {
      const a = d.anchor, b = this.t(x);
      this.range = selectRange(this.store.scene, this.endMarker(), Math.max(0, Math.min(a, b)), Math.max(a, b));
      this.refreshBar();
      this.draw();
      return;
    }
    if (d.kind === 'group') {
      const dt = this.snap(this.t(x)) - d.last;
      if (!dt || !this.range) return;
      const range = this.range;
      this.store.update((s) => {
        const applied = shiftSelection(s, range, dt, d.end);
        d.last += applied;
        if (d.end !== null) d.end += applied;
      }, 'group-move');
      return;
    }
    if (d.kind === 'marker') {
      const t = this.snap(this.t(x));
      const walk = this.walk();
      if (!walk) return;
      if (d.which === 'start') {
        this.store.update((s) => walk.setStart(s, t), 'marker-start');
      } else {
        const to = Math.max(walk.start + MIN_WALK, t);
        if (to === d.end) return;
        const from = d.end;
        this.store.update((s) => walk.stretch(s, from, to), 'marker-end');
        d.end = to;
      }
      return;
    }
    const lane = this.lanes().find((l) => l.id === d.sel.lane);
    if (!lane) return;
    const t = this.snap(this.t(x));
    const v = Math.max(lane.min, Math.min(lane.max, this.v(lane, y)));
    this.store.update((sc) => {
      const a = this.access(lane.id, sc);
      const k = a?.keys[d.sel.index];
      if (!a || !k) return;
      a.setAbs(k, t);
      a.setVal(k, v);
      this.resortSelected();
      d.sel.index = this.selKey!.index;
    }, `key-drag-${lane.id}`);
  }

  private up(): void {
    const d = this.drag;
    if (!d) return;
    if (d.kind === 'key' || d.kind === 'marker' || d.kind === 'group') this.store.endGesture();
    // A Cmd-click without a drag selects nothing.
    if (d.kind === 'range' && this.range && this.range.t1 - this.range.t0 < 0.01) this.range = null;
    this.drag = null;
    this.canvas.style.cursor = '';
    this.refreshBar();
    this.draw();
  }

  // Deletes the keys in the Cmd-dragged range (the start and end stay).
  private deleteRange(): void {
    const r = this.range;
    if (!r) return;
    this.store.update((sc) => {
      const L = sc.listener;
      L.speed = L.speed.filter((_, i) => !r.speed.includes(i));
      L.head.keys = L.head.keys.filter((_, i) => !r.head.includes(i));
    });
    this.range = null;
    this.refreshBar();
    this.draw();
  }

  private dblclick(e: MouseEvent): void {
    const [x, y] = this.local(e);
    const lane = this.laneAt(y);
    if (!lane || x < GUTTER || this.hitKey(x, y) || !this.access(lane.id)) return;
    const t = Math.max(0, Math.round(this.t(x) * 20) / 20);
    const v = Math.max(lane.min, Math.min(lane.max, this.v(lane, y)));
    const li = this.layerIndex();
    this.store.update((sc) => {
      if (lane.id === 'llevel' && !sc.layers[li].level_keys) sc.layers[li].level_keys = [];
      const a = this.access(lane.id, sc)!;
      const k = a.make(t, v);
      a.keys.push(k);
      sortKeys(a.keys);
      this.selKey = { lane: lane.id, index: a.keys.indexOf(k) };
    });
  }

  private deleteKey(): void {
    const s = this.selKey;
    if (!s) return;
    this.store.update((sc) => {
      this.access(s.lane, sc)?.keys.splice(s.index, 1);
      const l = sc.layers[this.layerIndex()];
      if (s.lane === 'llevel' && l?.level_keys && !l.level_keys.length) delete l.level_keys;
    });
    this.selKey = null;
  }

  private key(e: KeyboardEvent): void {
    const t = e.target as HTMLElement;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) return;
    // Plugin: Space is Logic's play/stop; left alone, it reaches Logic.
    if (e.code === 'Space') { if (this.backend.kind === 'plugin') return; this.togglePlay(); e.preventDefault(); return; }
    if ((e.key === 'Delete' || e.key === 'Backspace') && this.focused && (this.selKey || this.range)) {
      if (this.selKey) this.deleteKey(); else this.deleteRange();
      e.preventDefault();
      e.stopImmediatePropagation();
      return;
    }
    if (e.key === 'Escape' && this.range) {
      this.range = null;
      this.refreshBar();
      this.draw();
    }
  }

  private wheel(e: WheelEvent): void {
    e.preventDefault();
    const [x] = this.local(e);
    const span = this.t1 - this.t0;
    if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
      const d = ((e.shiftKey ? e.deltaY : e.deltaX) / (this.canvas.clientWidth - GUTTER)) * span;
      this.t0 = Math.max(0, this.t0 + d);
      this.t1 = this.t0 + span;
    } else {
      const f = Math.exp(e.deltaY * 0.002);
      const tc = this.t(x);
      const ns = Math.max(1, Math.min(this.store.duration * 1.5, span * f));
      this.t0 = Math.max(0, tc - ((tc - this.t0) / span) * ns);
      this.t1 = this.t0 + ns;
    }
    this.userZoomed = !(this.t0 <= 0 && Math.abs(this.t1 - this.store.duration) < 1e-6);
    this.draw();
  }

  get keyFocused(): boolean { return this.focused && (this.selKey !== null || this.range !== null); }
}

function niceStep(raw: number): number {
  const p = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 5, 10]) if (m * p >= raw) return m * p;
  return 10 * p;
}
