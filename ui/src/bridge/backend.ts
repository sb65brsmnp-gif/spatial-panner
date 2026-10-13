// The editor's connection to the native app. Inside the JUCE WebView it calls
// the app's native functions (app/Source/Bridge.cpp), or the plugin's
// (plugin/Source/PluginEditor.cpp: the host owns the transport, the tracks
// own the audio, the scene lives in the host's project); in a plain browser
// (npm run dev) it talks to the Vite dev server, which runs the engine's
// sp-scene tool for analysis and simulates the transport, so the editor can
// be developed and tested without the app.
import type { Analysis, AudioInfo } from '../model/store';
import { speakerLocal, type BassDoc, type SceneDoc, type SpeakerDoc, type V3 } from '../model/scene';

export type OutputMode = 'binaural' | 'speakers' | 'ambix';

// What the engine plays through. `speakers`: the scene's placed layout (the
// preset `layout` otherwise), each on its 1-based output channel.
export interface OutputConfig { mode: OutputMode; layout: string; speakers?: SpeakerDoc[]; bass?: BassDoc }

// Points and pins in the head's frame (metres; Y up, -Z ahead, +X right).
export interface PanGainsRequest {
  output: { speakers: SpeakerDoc[]; bass?: BassDoc };
  points: V3[];
  pins?: { position: V3; radius: number; gains_db: number[] }[];
  rolloff?: number;
  min_distance?: number;
  reference_distance?: number;
}

export interface EngineInfo {
  device: string;
  sampleRate: number;
  outputChannels: number;
  cpu: number;           // fraction of the audio callback budget
  // Plugin: what this instance renders to (set by its track's bus), with the
  // bus's channel names; `speakers` carries the bus's own positions or the
  // scene's placed layout on the channels its speakers name.
  output: OutputConfig & { channels?: string[] };
  status: string;        // human-readable engine state ("Playing", "Loading audio...")
  steamAudio?: boolean;  // the engine has the ray-traced room model (mesh rooms, objects)
  host?: 'plugin';
  tracks?: { id: string; name: string }[];  // plugin: the tracks running a layer instance
}

// Pushed at ~30 Hz by the app while it runs.
export interface Tick {
  time: number;
  playing: boolean;
  pose?: number[];       // [x, y, z, yaw, pitch, roll, distance, speed]
  meters?: number[];     // per-layer dBFS (post level, pre spatialisation)
  cpu?: number;
  host?: boolean;        // the host's playhead: follow it even while stopped
  // Plugin: the host is running the plug-in (false while Logic stands still,
  // unless the track is record-enabled or input-monitored); time and pose
  // are then the last ones seen.
  active?: boolean;
}

export interface BounceRequest extends OutputConfig { start: number; end: number; sampleRate: number }
// Progress of a running bounce; `done` with or without `error` at the end.
export interface BounceEvent { path: string; progress?: number; done?: boolean; error?: string }

export interface Backend {
  readonly kind: 'app' | 'dev' | 'plugin';
  analyze(scene: SceneDoc, duration: number): Promise<Analysis>;
  // `scene` is what the engine plays; `doc` the full document with editor keys
  // (the plugin stores it in the host's project).
  setScene(scene: SceneDoc, duration: number, doc?: SceneDoc): Promise<{ ok: boolean; error?: string }>;
  transport(cmd: { action: 'play' | 'pause' | 'stop' | 'seek' | 'loop'; time?: number; loop?: boolean }): Promise<void>;
  // Plugin: a text field has focus (or lost it); while it has, Logic's
  // transport keys are typed into it instead of going to Logic.
  editing(on: boolean): Promise<void>;
  // Plugin: the page's corner grip asks the host window for a new size of
  // the page area (CSS pixels); the plug-in clamps it to its limits.
  resize(width: number, height: number): Promise<void>;
  setOutput(out: OutputConfig): Promise<{ ok: boolean; error?: string }>;
  // Pink noise at -20 dBFS on one output channel (1-based; 0 stops it): the
  // device output in the app, this track's bus channel in the plugin (heard
  // while the host runs the plug-in).
  testSpeaker(channel: number): Promise<void>;
  // What the speaker panner does at head-local points: dB per speaker.
  panGains(req: PanGainsRequest): Promise<number[][]>;
  info(): Promise<EngineInfo>;
  // The native open dialog for audio files (several); `title` names the ask.
  chooseAudioFiles(title?: string): Promise<AudioInfo[]>;
  // One file of any kind through the native open dialog (e.g. an impulse
  // response WAV); `wildcard` as JUCE takes it ("*.wav;*.aif"). null if cancelled.
  chooseFile(title: string, wildcard: string): Promise<{ path: string; name: string } | null>;
  // A URL the page can load a picture from, given its path on disk.
  assetUrl(path: string): string;
  audioInfo(paths: string[]): Promise<AudioInfo[]>;
  openScene(): Promise<{ path: string; scene: SceneDoc; raw: unknown } | null>;
  saveScene(scene: SceneDoc, path: string | null): Promise<{ path: string } | null>;
  bounce(req: BounceRequest): Promise<{ path: string } | null>;
  showAudioSettings(): Promise<void>;
  // OK/Cancel question. The app's web view cannot show window.confirm on
  // macOS (it silently answers Cancel), so the native side asks.
  confirm(message: string, ok?: string): Promise<boolean>;
  revealFile(path: string): Promise<void>;
  onBounce(fn: (e: BounceEvent) => void): void;
  startupScene(): Promise<{ path: string; scene: SceneDoc; raw: unknown } | null>;
  onOpenFile(fn: (r: { path: string; scene: SceneDoc; raw: unknown }) => void): void;
  onTick(fn: (t: Tick) => void): void;
  onMessage(fn: (m: { text: string; level: 'info' | 'warning' | 'error' }) => void): void;
  // The native side changed the document (plugin: a track was added, renamed
  // or duplicated, or the host restored the project).
  onSceneReplaced(fn: (doc: SceneDoc) => void): void;
  // Recently opened or saved scene files, newest first.
  recentScenes(): Promise<string[]>;
  clearRecentScenes(): Promise<void>;
  // A scene file by path (Open Recent), read like openScene.
  openScenePath(path: string): Promise<{ path: string; scene: SceneDoc; raw: unknown } | null>;
  // Audio files dropped on the window (paths, where in the page).
  onDropFiles(fn: (d: { paths: string[]; x: number; y: number }) => void): void;
  onDropHover(fn: (over: boolean) => void): void;
  // The app's menu bar: new, open, save, saveAs, openRecent (with path).
  onMenu(fn: (m: { action: string; path?: string }) => void): void;
}

// ------------------------------------------------------------------ app

declare global {
  interface Window {
    __JUCE__?: {
      backend: {
        emitEvent(id: string, payload: unknown): void;
        addEventListener(id: string, fn: (payload: any) => void): unknown;
      };
      initialisationData: Record<string, unknown[]>;
    };
  }
}

class AppBackend implements Backend {
  readonly kind: 'app' | 'plugin';
  private nextId = 1;
  private pending = new Map<number, (v: unknown) => void>();

  constructor() {
    const host = window.__JUCE__!.initialisationData?.spHost;
    this.kind = (Array.isArray(host) ? host[0] : host) === 'plugin' ? 'plugin' : 'app';
    const b = window.__JUCE__!.backend;
    b.addEventListener('__juce__complete', ({ promiseId, result }: { promiseId: number; result: unknown }) => {
      const r = this.pending.get(promiseId);
      if (r) { this.pending.delete(promiseId); r(result); }
    });
  }

  // Same wire protocol as JUCE's @juce-framework/webview getNativeFunction.
  // Every native function returns a JSON string (or "" for nothing).
  private async call<T>(name: string, arg?: unknown): Promise<T> {
    const id = this.nextId++;
    const p = new Promise<unknown>((resolve) => this.pending.set(id, resolve));
    window.__JUCE__!.backend.emitEvent('__juce__invoke', { name, params: arg === undefined ? [] : [JSON.stringify(arg)], resultId: id });
    const raw = await p;
    if (typeof raw !== 'string' || raw === '') return null as T;
    const v = JSON.parse(raw);
    if (v && typeof v === 'object' && 'nativeError' in v) throw new Error(String(v.nativeError));
    return v as T;
  }

  analyze(scene: SceneDoc, duration: number) { return this.call<Analysis>('analyze', { scene, duration }); }
  setScene(scene: SceneDoc, duration: number, doc?: SceneDoc) {
    return this.call<{ ok: boolean; error?: string }>('setScene', doc ? { scene, duration, doc } : { scene, duration });
  }
  async transport(cmd: Parameters<Backend['transport']>[0]) { await this.call('transport', cmd); }
  async editing(on: boolean) { await this.call('editing', { on }); }
  async resize(width: number, height: number) { await this.call('resize', { width: Math.round(width), height: Math.round(height) }); }
  setOutput(out: OutputConfig) { return this.call<{ ok: boolean; error?: string }>('setOutput', out); }
  async testSpeaker(channel: number) { await this.call('testSpeaker', { channel }); }
  async panGains(req: PanGainsRequest) { return (await this.call<{ gains_db: number[][] }>('panGains', req))?.gains_db ?? []; }
  info() { return this.call<EngineInfo>('info'); }
  async chooseAudioFiles(title?: string) { return (await this.call<AudioInfo[]>('chooseAudioFiles', title ? { title } : {})) ?? []; }
  chooseFile(title: string, wildcard: string) { return this.call<{ path: string; name: string } | null>('chooseFile', { title, wildcard }); }
  // The native side serves files under /asset/<base64url of the path>; the
  // scheme hands it the URL path only, so the file path cannot be a query.
  assetUrl(path: string) {
    const b64 = btoa(unescape(encodeURIComponent(path))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    return new URL(`asset/${b64}`, window.location.href).toString();
  }
  async audioInfo(paths: string[]) { return (await this.call<AudioInfo[]>('audioInfo', { paths })) ?? []; }
  openScene() { return this.call<{ path: string; scene: SceneDoc; raw: unknown } | null>('openScene'); }
  saveScene(scene: SceneDoc, path: string | null) { return this.call<{ path: string } | null>('saveScene', { scene, path }); }
  bounce(req: BounceRequest) { return this.call<{ path: string } | null>('bounce', req); }
  async showAudioSettings() { await this.call('showAudioSettings'); }
  async confirm(message: string, ok?: string) {
    return !!(await this.call<{ ok: boolean } | null>('confirm', { message, ok: ok ?? 'OK' }))?.ok;
  }
  async revealFile(path: string) { await this.call('revealFile', { path }); }
  onBounce(fn: (e: BounceEvent) => void) {
    window.__JUCE__!.backend.addEventListener('bounce', (raw: unknown) => fn(typeof raw === 'string' ? JSON.parse(raw) : raw as BounceEvent));
  }
  startupScene() { return this.call<{ path: string; scene: SceneDoc; raw: unknown } | null>('startupScene'); }
  onOpenFile(fn: (r: { path: string; scene: SceneDoc; raw: unknown }) => void) {
    window.__JUCE__!.backend.addEventListener('openFile', (raw: string) => fn(typeof raw === 'string' ? JSON.parse(raw) : raw));
  }
  onTick(fn: (t: Tick) => void) { window.__JUCE__!.backend.addEventListener('tick', fn); }
  onMessage(fn: (m: { text: string; level: 'info' | 'warning' | 'error' }) => void) {
    window.__JUCE__!.backend.addEventListener('message', fn);
  }
  onSceneReplaced(fn: (doc: SceneDoc) => void) {
    window.__JUCE__!.backend.addEventListener('sceneReplaced', (raw: unknown) => fn(typeof raw === 'string' ? JSON.parse(raw) : raw as SceneDoc));
  }
  async recentScenes() { return (await this.call<string[]>('recentScenes')) ?? []; }
  async clearRecentScenes() { await this.call('clearRecentScenes'); }
  openScenePath(path: string) { return this.call<{ path: string; scene: SceneDoc; raw: unknown } | null>('openScenePath', { path }); }
  private on<T>(id: string, fn: (v: T) => void) {
    window.__JUCE__!.backend.addEventListener(id, (raw: unknown) => fn(typeof raw === 'string' ? JSON.parse(raw) : raw as T));
  }
  onDropFiles(fn: (d: { paths: string[]; x: number; y: number }) => void) { this.on('dropFiles', fn); }
  onDropHover(fn: (over: boolean) => void) { this.on<{ over: boolean }>('dropHover', (v) => fn(!!v.over)); }
  onMenu(fn: (m: { action: string; path?: string }) => void) { this.on('menu', fn); }
}

// ------------------------------------------------------------------ dev

class DevBackend implements Backend {
  readonly kind = 'dev' as const;
  private tickFns: ((t: Tick) => void)[] = [];
  private msgFns: ((m: { text: string; level: 'info' | 'warning' | 'error' }) => void)[] = [];
  private playing = false;
  private loop = false;
  private time = 0;
  private duration = 30;
  private startWall = 0;
  private startTime = 0;
  private output: OutputConfig = { mode: 'binaural', layout: '7.1.4' };

  constructor() {
    const step = () => {
      if (this.playing) {
        this.time = this.startTime + (performance.now() - this.startWall) / 1000;
        if (this.time >= this.duration) {
          if (this.loop) { this.startTime = 0; this.startWall = performance.now(); this.time = 0; }
          else { this.playing = false; this.time = this.duration; }
        }
      }
      for (const f of this.tickFns) f({ time: this.time, playing: this.playing });
      setTimeout(step, 33);
    };
    setTimeout(step, 33);
  }

  private async post<T>(url: string, body: unknown): Promise<T> {
    const r = await fetch(url, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
    return r.json();
  }

  analyze(scene: SceneDoc, duration: number) { return this.post<Analysis>('/api/analyze', { scene, duration }); }
  async setScene(_scene: SceneDoc, duration: number) { this.duration = duration; return { ok: true }; }
  async transport(cmd: Parameters<Backend['transport']>[0]) {
    if (cmd.loop !== undefined) this.loop = cmd.loop;
    switch (cmd.action) {
      case 'play': this.playing = true; this.startTime = this.time >= this.duration ? 0 : this.time; this.startWall = performance.now(); break;
      case 'pause': this.playing = false; break;
      case 'stop': this.playing = false; this.time = 0; break;
      case 'seek': this.time = cmd.time ?? 0; this.startTime = this.time; this.startWall = performance.now(); break;
      default: break;
    }
  }
  async editing(on: boolean) {
    (window as unknown as { __editing?: boolean }).__editing = on;
  }
  async resize() { /* the browser window is the user's to size */ }
  async setOutput(out: OutputConfig) { this.output = out; return { ok: true }; }
  async testSpeaker(_channel: number) { /* no audio in the preview */ }
  // The preview has no engine: a rough stand-in for VBAP (the nearest
  // speakers by direction, cosine-weighted), with the same distance law,
  // alignment and pin blending as the engine's probe.
  async panGains(req: PanGainsRequest) { return approxPanGains(req); }
  async info(): Promise<EngineInfo> {
    return { device: 'Browser preview (no audio)', sampleRate: 48000, outputChannels: 2, cpu: 0, output: this.output, status: 'Preview' };
  }
  async chooseAudioFiles() {
    const r = await fetch('/api/signals');
    return (await r.json()) as AudioInfo[];
  }
  async chooseFile(title: string) {
    const path = window.prompt(`${title}\nPath to a file (browser preview: nothing is read):`, '');
    return path ? { path, name: path.split(/[\\/]/).pop() ?? path } : null;
  }
  // The browser preview loads pictures by URL (a path the dev server serves).
  assetUrl(path: string) { return path; }
  async audioInfo(paths: string[]) {
    return paths.map((p) => ({ path: p, name: p.split('/').pop() ?? p, duration: 6, channels: 1, sampleRate: 48000 }));
  }
  async openScene() {
    const list: string[] = await (await fetch('/api/scenes')).json();
    const choice = window.prompt(`Open which demo scene?\n${list.join('\n')}`, list[0]);
    if (!choice) return null;
    this.noteRecent(choice);
    return this.post<{ path: string; scene: SceneDoc; raw: unknown }>('/api/open', { name: choice });
  }
  async saveScene(scene: SceneDoc, path: string | null) {
    const blob = new Blob([JSON.stringify(scene, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = (path?.split('/').pop()) || 'scene.json';
    a.click();
    return { path: a.download };
  }
  async bounce() {
    for (const f of this.msgFns) f({ text: 'Bouncing needs the app (the browser preview has no audio engine).', level: 'warning' });
    return null;
  }
  async showAudioSettings() {
    for (const f of this.msgFns) f({ text: 'Audio settings are in the app.', level: 'info' });
  }
  async confirm(message: string) { return window.confirm(message); }
  async revealFile() {}
  onBounce() {}
  async startupScene() {
    const name = new URLSearchParams(location.search).get('scene');
    return name ? this.post<{ path: string; scene: SceneDoc; raw: unknown }>('/api/open', { name }) : null;
  }
  onOpenFile() {}
  onTick(fn: (t: Tick) => void) { this.tickFns.push(fn); }
  onMessage(fn: (m: { text: string; level: 'info' | 'warning' | 'error' }) => void) { this.msgFns.push(fn); }
  onSceneReplaced() {}
  // The browser preview keeps a recent list of the demo scenes it opened.
  private recent: string[] = [];
  async recentScenes() { return this.recent.slice(); }
  async clearRecentScenes() { this.recent = []; }
  async openScenePath(path: string) {
    const r = await this.post<{ path: string; scene: SceneDoc; raw: unknown }>('/api/open', { name: path.split('/').pop() });
    this.noteRecent(path);
    return r;
  }
  noteRecent(path: string) { this.recent = [path, ...this.recent.filter((p) => p !== path)].slice(0, 10); }
  // A browser cannot see a dropped file's path: the editor's own drop
  // handler passes names (main.ts), which the preview treats as paths.
  onDropFiles() {}
  onDropHover() {}
  onMenu() {}
}

export function createBackend(): Backend {
  return window.__JUCE__?.backend ? new AppBackend() : new DevBackend();
}

export function approxPanGains(req: PanGainsRequest): number[][] {
  const spk = req.output.speakers;
  const dirs = spk.map((x) => { const v = speakerLocal(x, 1); const l = Math.hypot(...v) || 1; return [v[0] / l, v[1] / l, v[2] / l]; });
  const maxD = Math.max(0, ...spk.map((x) => x.distance));
  const align = spk.map((x) => (maxD > 0 ? (x.distance > 0 ? x.distance : maxD) / maxD : 1) * Math.pow(10, (x.trim_db ?? 0) / 20));
  const rolloff = req.rolloff ?? 1, minD = req.min_distance ?? 0.25, refD = req.reference_distance ?? 1;
  const refGain = Math.pow(Math.max(refD, 0.01), rolloff);
  const smooth = (x: number) => { x = Math.max(0, Math.min(1, x)); return x * x * (3 - 2 * x); };
  return req.points.map((p) => {
    const dist = Math.hypot(...p);
    const d = dist > 1e-6 ? [p[0] / dist, p[1] / dist, p[2] / dist] : [0, 0, -1];
    let g = dirs.map((v, i) => (spk[i].lfe ? 0 : Math.pow(Math.max(0, v[0] * d[0] + v[1] * d[1] + v[2] * d[2]), 8)));
    const power = Math.sqrt(g.reduce((a, b) => a + b * b, 0)) || 1;
    g = g.map((x) => x / power);
    // Pins, as the engine's applyPins.
    let total = 0;
    const mix = new Array(spk.length).fill(0);
    for (const pin of req.pins ?? []) {
      const dd = Math.hypot(p[0] - pin.position[0], p[1] - pin.position[1], p[2] - pin.position[2]);
      const w = smooth(1 - dd / Math.max(pin.radius, 1e-3));
      if (w <= 0) continue;
      total += w;
      pin.gains_db.forEach((db, c) => { if (c < mix.length && db > -119) mix[c] += w * Math.pow(10, db / 20); });
    }
    if (total > 0) { const share = Math.min(total, 1); g = g.map((x, c) => (1 - share) * x + share * mix[c] / total); }
    const distGain = refGain / Math.pow(Math.max(dist, minD), rolloff);
    return g.map((x, i) => { const lin = spk[i].lfe ? 0 : x * distGain * align[i]; return lin > 1e-6 ? Math.max(-120, 20 * Math.log10(lin)) : -120; });
  });
}
