// Timeline under the viewport: transport, playhead/scrubbing, and keyframe
// lanes for the listener's speed along the path and head yaw and pitch.
//
// Keys: drag to move, double-click a lane to add, Delete to remove, the key
// bar edits the selected key's exact values and easing.
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
import { EASINGS, headKeyAt, sortKeys, speedAt, type Easing, type HeadKey, type SceneDoc, type SpeedKey } from '../model/scene';
import type { Backend } from '../bridge/backend';
import { el, fmtTime, numberInput, select } from './dom';

type LaneId = 'speed' | 'yaw' | 'pitch';
interface Lane { id: LaneId; label: string; y: number; h: number; min: number; max: number; unit: string }
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
    store.subscribe(() => { this.refreshBar(); this.draw(); });
    this.refreshBar();
  }

  // ------------------------------------------------------------ layout

  private lanes(): Lane[] {
    const h = this.canvas.clientHeight - RULER;
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
    const L = this.store.scene.listener;
    if (lane === 'speed') return L.speed.map((k) => ({ t: speedKeyAbsolute(this.store.scene, k.time), v: k.speed }));
    return L.head.keys.map((k) => ({ t: k.time, v: lane === 'yaw' ? k.yaw : k.pitch }));
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
      ctx.fillText(`${lane.max}${lane.unit}`, 8, lane.y + 30);
      ctx.fillText(`${lane.min}${lane.unit}`, 8, lane.y + lane.h - 6);
      if (lane.id !== 'speed') {
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

    if (L.position_mode === 'along_path') {
      const lane = this.lanes()[0];
      ctx.fillStyle = 'rgba(24,27,33,0.8)';
      ctx.fillRect(GUTTER, lane.y + 1, w - GUTTER, lane.h - 1);
      ctx.fillStyle = '#9aa3b2';
      ctx.fillText('Position is set by "Position along path" (Path tab), not by speed.', GUTTER + 10, lane.y + lane.h / 2);
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
    const start = pathStartTime(this.store.scene);
    if (this.hasWalk()) {
      const lane = this.lanes()[0];
      const sx = Math.min(w, this.x(start));
      if (sx > GUTTER) {
        ctx.fillStyle = 'rgba(24,27,33,0.6)';
        ctx.fillRect(GUTTER, lane.y + 1, sx - GUTTER, lane.h - 1);
      }
      this.marker(sx, '#5fd38d', 'path start', !!range?.start);
    }

    // Arrival at the end of the path: the red line (drag it to make the walk
    // faster or slower).
    const end = this.endMarker();
    if (end !== null) this.marker(this.x(end), '#eb5757', 'path end', !!range?.end);


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
    const a = this.store.analysis;
    return this.hasWalk() && a && a.arrival_time > 0 ? a.arrival_time : null;
  }

  // The start or end line under `x`, if any.
  private markerAt(x: number): 'start' | 'end' | null {
    if (!this.hasWalk()) return null;
    const end = this.endMarker();
    if (end !== null && Math.abs(this.x(end) - x) < 6) return 'end';
    if (Math.abs(this.x(pathStartTime(this.store.scene)) - x) < 6) return 'start';
    return null;
  }

  private drawLaneCurves(lane: Lane): void {
    const ctx = this.ctx;
    const w = this.canvas.clientWidth;
    const L = this.store.scene.listener;
    const a = this.store.analysis;
    // Resulting head angle from the engine (faint), so relative keys can be
    // read against where the head actually points.
    if (lane.id !== 'speed' && a && a.poses.length) {
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
    const color = lane.id === 'speed' ? '#5fd38d' : lane.id === 'yaw' ? '#56ccf2' : '#bb6bd9';
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let x = GUTTER; x <= w; x += 2) {
      const t = this.t(x);
      let v: number;
      if (lane.id === 'speed') v = speedAt(L.speed, t - pathStartTime(this.store.scene));
      else {
        if (!L.head.keys.length) { v = 0; }
        else { const h = headKeyAt(L.head.keys, t); v = lane.id === 'yaw' ? h.yaw : h.pitch; }
      }
      const y = this.y(lane, Math.max(lane.min, Math.min(lane.max, v)));
      if (x === GUTTER) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.lineWidth = 1;
    // Keys.
    this.keysFor(lane.id).forEach((k, i) => {
      const x = this.x(k.t), y = this.y(lane, Math.max(lane.min, Math.min(lane.max, k.v)));
      const sel = (this.selKey && this.selKey.index === i && (this.selKey.lane === lane.id || (lane.id !== 'speed' && this.selKey.lane !== 'speed')))
        || (this.range && (lane.id === 'speed' ? this.range.speed : this.range.head).includes(i));
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
    const L = this.store.scene.listener;
    const key = s ? (s.lane === 'speed' ? L.speed[s.index] : L.head.keys[s.index]) : null;
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
      this.keyBar.append(el('span', { class: 'hint' }, this.hasWalk()
        ? 'Double-click a lane to add a key; drag the start and end lines; Cmd-drag a range to move everything in it'
        : 'Double-click a lane to add a key'));
      return;
    }
    const upd = (fn: () => void) => this.store.update(() => { fn(); this.resortSelected(); }, `key-edit-${s.lane}-${s.index}`);
    const fromStart = s.lane === 'speed' && pathStartTime(this.store.scene) > 0;
    this.keyBar.append(el('span', { class: 'tl-sep' }, 'Key at'),
      numberInput(key.time, (v) => upd(() => { key.time = Math.max(0, v); }), { step: 0.1, width: 56 }),
      el('span', { class: 'tl-unit' }, fromStart ? 's after the path starts' : 's'));
    if (s.lane === 'speed') {
      const k = key as SpeedKey;
      this.keyBar.append(numberInput(k.speed, (v) => upd(() => { k.speed = Math.max(0, v); }), { step: 0.1, width: 52, def: 1.4 }),
        el('span', { class: 'tl-unit' }, 'm/s'));
    } else {
      const k = key as HeadKey;
      this.keyBar.append(el('span', { class: 'tl-sep' }, 'Yaw'), numberInput(k.yaw, (v) => upd(() => { k.yaw = v; }), { step: 1, width: 52, def: 0 }),
        el('span', { class: 'tl-sep' }, 'Pitch'), numberInput(k.pitch, (v) => upd(() => { k.pitch = Math.max(-90, Math.min(90, v)); }), { step: 1, width: 52, def: 0 }));
    }
    this.keyBar.append(el('span', { class: 'tl-sep' }, 'then'),
      select(EASINGS, key.easing, (v) => upd(() => { key.easing = v as Easing; }), { linear: 'linear', smooth: 'smooth', ease_in: 'ease in', ease_out: 'ease out', hold: 'hold' }, 'linear'));
    const del = el('button', { class: 'tbtn small', title: 'Delete key (Delete)' }, '✕');
    del.addEventListener('click', () => this.deleteKey());
    this.keyBar.append(del);
  }

  private resortSelected(): void {
    const s = this.selKey;
    if (!s) return;
    const L = this.store.scene.listener;
    const arr: { time: number }[] = s.lane === 'speed' ? L.speed : L.head.keys;
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
    if (e.metaKey || e.ctrlKey) {
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
    if (hit && !(hit.lane === 'speed' && this.store.scene.listener.position_mode === 'along_path')) {
      this.selKey = hit;
      if (e.altKey) {
        // Option-click: the key's value back to its default (time stays).
        const L = this.store.scene.listener;
        this.store.update(() => {
          if (hit.lane === 'speed') L.speed[hit.index].speed = 1.4;
          else if (hit.lane === 'yaw') L.head.keys[hit.index].yaw = 0;
          else L.head.keys[hit.index].pitch = 0;
        });
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
      if (d.which === 'start') {
        this.store.update((s) => { s.listener.path_start_time = t; }, 'marker-start');
      } else {
        const to = Math.max(pathStartTime(this.store.scene) + MIN_WALK, t);
        if (to === d.end) return;
        const from = d.end;
        this.store.update((s) => stretchPathEnd(s, from, to), 'marker-end');
        d.end = to;
      }
      return;
    }
    const lane = this.lanes().find((l) => l.id === d.sel.lane)!;
    const t = this.snap(this.t(x));
    const v = Math.max(lane.min, Math.min(lane.max, this.v(lane, y)));
    const L = this.store.scene.listener;
    this.store.update(() => {
      if (lane.id === 'speed') {
        const k = L.speed[d.sel.index];
        k.time = speedKeyTime(this.store.scene, t);
        k.speed = Math.round(v * 20) / 20;
      } else {
        const k = L.head.keys[d.sel.index];
        k.time = t;
        if (lane.id === 'yaw') k.yaw = Math.round(v); else k.pitch = Math.round(v);
      }
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
    if (!lane || x < GUTTER || this.hitKey(x, y)) return;
    const L = this.store.scene.listener;
    if (lane.id === 'speed' && L.position_mode === 'along_path') return;
    const t = Math.max(0, Math.round(this.t(x) * 20) / 20);
    const v = this.v(lane, y);
    this.store.update(() => {
      if (lane.id === 'speed') {
        const easing: Easing = L.speed.length ? L.speed[0].easing : 'linear';
        const k: SpeedKey = { time: speedKeyTime(this.store.scene, t), speed: Math.max(0, Math.round(v * 20) / 20), easing };
        L.speed.push(k);
        sortKeys(L.speed);
        this.selKey = { lane: 'speed', index: L.speed.indexOf(k) };
      } else {
        const cur = headKeyAt(L.head.keys, t);
        const k: HeadKey = { time: t, yaw: lane.id === 'yaw' ? Math.round(v) : Math.round(cur.yaw),
          pitch: lane.id === 'pitch' ? Math.round(v) : Math.round(cur.pitch), roll: cur.roll, easing: 'smooth' };
        L.head.keys.push(k);
        sortKeys(L.head.keys);
        this.selKey = { lane: lane.id, index: L.head.keys.indexOf(k) };
      }
    });
  }

  private deleteKey(): void {
    const s = this.selKey;
    if (!s) return;
    this.store.update((sc) => {
      if (s.lane === 'speed') sc.listener.speed.splice(s.index, 1);
      else sc.listener.head.keys.splice(s.index, 1);
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
