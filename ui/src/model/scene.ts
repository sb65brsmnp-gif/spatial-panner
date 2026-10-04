// The scene document: exactly the engine's scene JSON (docs/engine.md), plus
// a few editor-only keys the engine ignores (layer colour and solo, the
// top-level "editor" block). The engine's sceneToJson() output is the
// canonical form; the native side normalises files to it when they open.

export type V3 = [number, number, number];
export type Easing = 'linear' | 'smooth' | 'ease_in' | 'ease_out' | 'hold';
export const EASINGS: Easing[] = ['linear', 'smooth', 'ease_in', 'ease_out', 'hold'];

export interface LayerDoc {
  name: string;
  audio: string;
  position: V3;
  level_db: number;
  mute: boolean;
  doppler: number;
  spread_deg: number;
  directivity: number;
  directivity_forward: V3;
  reference_distance: number;
  min_distance: number;
  rolloff: number;
  reverb_send_db: number;
  reflection_order: number;
  start_time: number;
  loop: boolean;
  // editor-only
  color?: string;
  solo?: boolean;
}

export type SegmentType = 'line' | 'bezier' | 'catmull_rom' | 'arc';

export interface SegmentDoc {
  type: SegmentType;
  points: V3[];
  turns?: number;
  clockwise?: boolean;
}

export interface PathDoc {
  name: string;
  closed: boolean;
  segments: SegmentDoc[];
}

export interface SpeedKey { time: number; speed: number; easing: Easing }
export interface HeadKey { time: number; yaw: number; pitch: number; roll: number; easing: Easing }

export type HeadMode = 'along_path' | 'look_at' | 'keyframed';

export interface HeadDoc {
  mode: HeadMode;
  banking: boolean;
  look_at_point: V3;
  look_at_layer: number;
  keys: HeadKey[];
  yaw_offset: number;
  pitch_offset: number;
  roll_offset: number;
}

export interface ListenerDoc {
  paths: PathDoc[];
  active_path: number;
  position_mode: 'speed' | 'along_path';
  speed: SpeedKey[];
  path_start_time: number;
  path_fraction: number;
  loop_path: boolean;
  static_position: V3;
  head_radius: number;
  head: HeadDoc;
}

export interface MaterialDoc { name: string; absorption?: number[] }

export const WALLS = ['left', 'right', 'floor', 'ceiling', 'front', 'back'] as const;
export type WallKey = typeof WALLS[number];

export interface RoomDoc {
  type: 'box' | 'outdoor' | 'none';
  size: V3;
  origin: V3;
  materials: Record<WallKey, MaterialDoc>;
  reflection_order: number;
  reflections_level_db: number;
  reverb_level_db: number;
  reverb_time_scale: number;
  reflections: boolean;
  reverb: boolean;
}

export interface EnvironmentDoc {
  speed_of_sound: number;
  temperature_c: number;
  humidity: number;
  pressure_kpa: number;
  air_absorption: boolean;
}

export interface EditorMeta {
  draw_height?: number;
  output?: { mode: 'binaural' | 'speakers' | 'ambix'; layout: string };
}

export interface SceneDoc {
  name: string;
  duration: number;
  layers: LayerDoc[];
  room: RoomDoc;
  listener: ListenerDoc;
  environment: EnvironmentDoc;
  editor?: EditorMeta;
}

// Engine material names (engine/src/Materials.cpp).
export const MATERIALS = [
  'concrete', 'brick', 'plaster', 'glass', 'wood_panel', 'wood_floor', 'carpet', 'curtain',
  'acoustic_tile', 'absorber', 'grass', 'gravel', 'asphalt', 'water', 'snow', 'audience',
];

export const LAYOUTS = ['stereo', 'quad', '5.1', '7.1', '5.1.4', '7.1.4', '9.1.6'];

const PALETTE = ['#4f9cf9', '#f97f4f', '#5fd38d', '#e05fd3', '#f2c94c', '#56ccf2', '#eb5757', '#9b8cff',
  '#6fcf97', '#f2994a', '#bb6bd9', '#2fd1c5'];

export function layerColor(index: number): string {
  return PALETTE[index % PALETTE.length];
}

export function defaultLayer(index: number, partial: Partial<LayerDoc> = {}): LayerDoc {
  return {
    name: `Layer ${index + 1}`,
    audio: '',
    position: [0, 1.6, -2],
    level_db: 0,
    mute: false,
    doppler: 1,
    spread_deg: 0,
    directivity: 0,
    directivity_forward: [0, 0, 1],
    reference_distance: 1,
    min_distance: 0.25,
    rolloff: 1,
    reverb_send_db: 0,
    reflection_order: -1,
    start_time: 0,
    loop: true,
    color: layerColor(index),
    ...partial,
  };
}

export function defaultHead(): HeadDoc {
  return {
    mode: 'along_path', banking: false, look_at_point: [0, 1.6, 0], look_at_layer: -1, keys: [],
    yaw_offset: 0, pitch_offset: 0, roll_offset: 0,
  };
}

export function defaultRoom(): RoomDoc {
  const m = (name: string): MaterialDoc => ({ name });
  return {
    type: 'box', size: [12, 3.5, 16], origin: [0, 0, 0],
    materials: { left: m('plaster'), right: m('plaster'), floor: m('wood_floor'), ceiling: m('plaster'),
      front: m('plaster'), back: m('plaster') },
    reflection_order: 2, reflections_level_db: 0, reverb_level_db: 0, reverb_time_scale: 1,
    reflections: true, reverb: true,
  };
}

export function defaultScene(): SceneDoc {
  return {
    name: 'Untitled scene',
    duration: 0,
    layers: [],
    room: defaultRoom(),
    listener: {
      paths: [], active_path: 0, position_mode: 'speed',
      speed: [{ time: 0, speed: 1.4, easing: 'linear' }],
      path_start_time: 0, path_fraction: 0, loop_path: false, static_position: [0, 1.7, 0],
      head_radius: 0.0875, head: defaultHead(),
    },
    environment: { speed_of_sound: 343, temperature_c: 20, humidity: 50, pressure_kpa: 101.325, air_absorption: true },
    editor: { draw_height: 1.7, output: { mode: 'binaural', layout: '7.1.4' } },
  };
}

// Fills in anything missing (older files, hand-written JSON) from the defaults.
export function completeScene(raw: Partial<SceneDoc>): SceneDoc {
  const d = defaultScene();
  const s: SceneDoc = { ...d, ...raw } as SceneDoc;
  s.room = { ...d.room, ...(raw.room ?? {}) } as RoomDoc;
  s.room.materials = { ...d.room.materials, ...(raw.room?.materials ?? {}) };
  s.listener = { ...d.listener, ...(raw.listener ?? {}) } as ListenerDoc;
  s.listener.head = { ...d.listener.head, ...(raw.listener?.head ?? {}) };
  s.environment = { ...d.environment, ...(raw.environment ?? {}) };
  s.layers = (raw.layers ?? []).map((l, i) => defaultLayer(i, l));
  s.editor = { ...d.editor, ...(raw.editor ?? {}) };
  return s;
}

// Keeps editor-only keys (colour, solo, "editor") from `previous` when the
// native side hands back the canonical form of the same file.
export function mergeEditorKeys(canonical: SceneDoc, raw: any): SceneDoc {
  const s = completeScene(canonical);
  if (raw && typeof raw === 'object') {
    if (raw.editor) s.editor = { ...s.editor, ...raw.editor };
    if (Array.isArray(raw.layers))
      s.layers.forEach((l, i) => {
        const r = raw.layers[i];
        if (r?.color) l.color = r.color;
        if (r?.solo) l.solo = true;
      });
  }
  return s;
}

// What the engine plays: solo turns into mute for the others.
export function engineScene(s: SceneDoc): SceneDoc {
  const anySolo = s.layers.some((l) => l.solo);
  if (!anySolo) return s;
  return { ...s, layers: s.layers.map((l) => ({ ...l, mute: l.mute || !l.solo })) };
}

export function applyEasing(e: Easing, u: number): number {
  u = Math.min(1, Math.max(0, u));
  switch (e) {
    case 'linear': return u;
    case 'smooth': return u * u * (3 - 2 * u);
    case 'ease_in': return u * u;
    case 'ease_out': return 1 - (1 - u) * (1 - u);
    case 'hold': return 0;
  }
  return u;
}

// Same as SampledSpeed::evalSpeed.
export function speedAt(keys: SpeedKey[], t: number): number {
  if (keys.length === 0) return 1.4;
  if (t <= keys[0].time) return Math.max(0, keys[0].speed);
  const last = keys[keys.length - 1];
  if (t >= last.time) return Math.max(0, last.speed);
  let i = 0;
  while (i + 1 < keys.length && keys[i + 1].time <= t) i++;
  const a = keys[i], b = keys[i + 1];
  const span = b.time - a.time;
  const u = span > 0 ? (t - a.time) / span : 1;
  return Math.max(0, a.speed + (b.speed - a.speed) * applyEasing(a.easing, u));
}

export function headKeyAt(keys: HeadKey[], t: number): { yaw: number; pitch: number; roll: number } {
  if (keys.length === 0) return { yaw: 0, pitch: 0, roll: 0 };
  const first = keys[0], last = keys[keys.length - 1];
  if (t <= first.time) return { yaw: first.yaw, pitch: first.pitch, roll: first.roll };
  if (t >= last.time) return { yaw: last.yaw, pitch: last.pitch, roll: last.roll };
  let i = 0;
  while (i + 1 < keys.length && keys[i + 1].time <= t) i++;
  const a = keys[i], b = keys[i + 1];
  const span = b.time - a.time;
  const u = applyEasing(a.easing, span > 0 ? (t - a.time) / span : 1);
  return { yaw: a.yaw + (b.yaw - a.yaw) * u, pitch: a.pitch + (b.pitch - a.pitch) * u, roll: a.roll + (b.roll - a.roll) * u };
}

export function sortKeys<T extends { time: number }>(keys: T[]): void {
  keys.sort((a, b) => a.time - b.time);
}
