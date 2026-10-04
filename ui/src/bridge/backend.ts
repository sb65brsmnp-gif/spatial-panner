// The editor's connection to the native app. Inside the JUCE WebView it calls
// the app's native functions (app/Source/Bridge.cpp); in a plain browser
// (npm run dev) it talks to the Vite dev server, which runs the engine's
// sp-scene tool for analysis and simulates the transport, so the editor can
// be developed and tested without the app.
import type { Analysis, AudioInfo } from '../model/store';
import type { SceneDoc } from '../model/scene';

export type OutputMode = 'binaural' | 'speakers' | 'ambix';

export interface OutputConfig { mode: OutputMode; layout: string }

export interface EngineInfo {
  device: string;
  sampleRate: number;
  outputChannels: number;
  cpu: number;           // fraction of the audio callback budget
  output: OutputConfig;
  status: string;        // human-readable engine state ("Playing", "Loading audio...")
}

// Pushed at ~30 Hz by the app while it runs.
export interface Tick {
  time: number;
  playing: boolean;
  pose?: number[];       // [x, y, z, yaw, pitch, roll, distance, speed]
  meters?: number[];     // per-layer dBFS (post level, pre spatialisation)
  cpu?: number;
}

export interface BounceRequest { mode: OutputMode; layout: string; start: number; end: number; sampleRate: number }

export interface Backend {
  readonly kind: 'app' | 'dev';
  analyze(scene: SceneDoc, duration: number): Promise<Analysis>;
  setScene(scene: SceneDoc, duration: number): Promise<{ ok: boolean; error?: string }>;
  transport(cmd: { action: 'play' | 'pause' | 'stop' | 'seek' | 'loop'; time?: number; loop?: boolean }): Promise<void>;
  setOutput(out: OutputConfig): Promise<{ ok: boolean; error?: string }>;
  info(): Promise<EngineInfo>;
  chooseAudioFiles(): Promise<AudioInfo[]>;
  audioInfo(paths: string[]): Promise<AudioInfo[]>;
  openScene(): Promise<{ path: string; scene: SceneDoc; raw: unknown } | null>;
  saveScene(scene: SceneDoc, path: string | null): Promise<{ path: string } | null>;
  bounce(req: BounceRequest): Promise<{ path: string } | null>;
  showAudioSettings(): Promise<void>;
  startupScene(): Promise<{ path: string; scene: SceneDoc; raw: unknown } | null>;
  onOpenFile(fn: (r: { path: string; scene: SceneDoc; raw: unknown }) => void): void;
  onTick(fn: (t: Tick) => void): void;
  onMessage(fn: (m: { text: string; level: 'info' | 'warning' | 'error' }) => void): void;
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
  readonly kind = 'app' as const;
  private nextId = 1;
  private pending = new Map<number, (v: unknown) => void>();

  constructor() {
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
  setScene(scene: SceneDoc, duration: number) { return this.call<{ ok: boolean; error?: string }>('setScene', { scene, duration }); }
  async transport(cmd: Parameters<Backend['transport']>[0]) { await this.call('transport', cmd); }
  setOutput(out: OutputConfig) { return this.call<{ ok: boolean; error?: string }>('setOutput', out); }
  info() { return this.call<EngineInfo>('info'); }
  async chooseAudioFiles() { return (await this.call<AudioInfo[]>('chooseAudioFiles')) ?? []; }
  async audioInfo(paths: string[]) { return (await this.call<AudioInfo[]>('audioInfo', { paths })) ?? []; }
  openScene() { return this.call<{ path: string; scene: SceneDoc; raw: unknown } | null>('openScene'); }
  saveScene(scene: SceneDoc, path: string | null) { return this.call<{ path: string } | null>('saveScene', { scene, path }); }
  bounce(req: BounceRequest) { return this.call<{ path: string } | null>('bounce', req); }
  async showAudioSettings() { await this.call('showAudioSettings'); }
  startupScene() { return this.call<{ path: string; scene: SceneDoc; raw: unknown } | null>('startupScene'); }
  onOpenFile(fn: (r: { path: string; scene: SceneDoc; raw: unknown }) => void) {
    window.__JUCE__!.backend.addEventListener('openFile', (raw: string) => fn(typeof raw === 'string' ? JSON.parse(raw) : raw));
  }
  onTick(fn: (t: Tick) => void) { window.__JUCE__!.backend.addEventListener('tick', fn); }
  onMessage(fn: (m: { text: string; level: 'info' | 'warning' | 'error' }) => void) {
    window.__JUCE__!.backend.addEventListener('message', fn);
  }
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
  async setOutput(out: OutputConfig) { this.output = out; return { ok: true }; }
  async info(): Promise<EngineInfo> {
    return { device: 'Browser preview (no audio)', sampleRate: 48000, outputChannels: 2, cpu: 0, output: this.output, status: 'Preview' };
  }
  async chooseAudioFiles() {
    const r = await fetch('/api/signals');
    return (await r.json()) as AudioInfo[];
  }
  async audioInfo(paths: string[]) {
    return paths.map((p) => ({ path: p, name: p.split('/').pop() ?? p, duration: 6, channels: 1, sampleRate: 48000 }));
  }
  async openScene() {
    const list: string[] = await (await fetch('/api/scenes')).json();
    const choice = window.prompt(`Open which demo scene?\n${list.join('\n')}`, list[0]);
    if (!choice) return null;
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
  async startupScene() {
    const name = new URLSearchParams(location.search).get('scene');
    return name ? this.post<{ path: string; scene: SceneDoc; raw: unknown }>('/api/open', { name }) : null;
  }
  onOpenFile() {}
  onTick(fn: (t: Tick) => void) { this.tickFns.push(fn); }
  onMessage(fn: (m: { text: string; level: 'info' | 'warning' | 'error' }) => void) { this.msgFns.push(fn); }
}

export function createBackend(): Backend {
  return window.__JUCE__?.backend ? new AppBackend() : new DevBackend();
}
