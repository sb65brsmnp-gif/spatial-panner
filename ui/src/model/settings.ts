// Editor display settings (Settings tab): label text size and distance from
// the balls, meter sizes, dot size. Kept in the browser's storage, not the
// scene, so they follow the user rather than the file.
export type LabelMode = 'always' | 'selected' | 'off';

export interface Settings {
  labelSize: number;      // px
  labelOffset: number;    // metres above a layer's ball
  labelOpacity: number;   // 0..1, the label's background
  labelMaxWidth: number;  // px; longer names are cut with an ellipsis
  labelMode: LabelMode;
  meterWidth: number;     // px, the Layers list
  meterHeight: number;    // px, list and 3D labels
  dotSize: number;        // scale of the layer balls and path points
  timeDots: boolean;      // the second-marks along the listener's path
  speakerLabels: boolean;
}

export const DEFAULT_SETTINGS: Settings = {
  labelSize: 11, labelOffset: 0.25, labelOpacity: 0.8, labelMaxWidth: 220, labelMode: 'always',
  meterWidth: 50, meterHeight: 4, dotSize: 1, timeDots: true, speakerLabels: true,
};

const KEY = 'sp.settings';

export function loadSettings(storage: Pick<Storage, 'getItem'> | null = safeStorage()): Settings {
  const out = { ...DEFAULT_SETTINGS };
  try {
    const raw = storage?.getItem(KEY);
    const v = raw ? JSON.parse(raw) : null;
    if (v && typeof v === 'object') {
      for (const k of Object.keys(DEFAULT_SETTINGS) as (keyof Settings)[]) {
        const x = (v as Record<string, unknown>)[k];
        if (typeof x === typeof DEFAULT_SETTINGS[k] && (typeof x !== 'number' || Number.isFinite(x))) (out as Record<string, unknown>)[k] = x;
      }
      if (!['always', 'selected', 'off'].includes(out.labelMode)) out.labelMode = 'always';
    }
  } catch { /* unreadable: defaults */ }
  return out;
}

export function saveSettings(s: Settings, storage: Pick<Storage, 'setItem'> | null = safeStorage()): void {
  try { storage?.setItem(KEY, JSON.stringify(s)); } catch { /* storage full or off */ }
}

// The page-wide part: CSS variables the labels and meters read.
export function applySettingsToPage(s: Settings, root: HTMLElement = document.documentElement): void {
  root.style.setProperty('--label-size', `${s.labelSize}px`);
  root.style.setProperty('--label-opacity', String(s.labelOpacity));
  root.style.setProperty('--label-max', `${s.labelMaxWidth}px`);
  root.style.setProperty('--meter-w', `${s.meterWidth}px`);
  root.style.setProperty('--meter-h', `${s.meterHeight}px`);
}

function safeStorage(): Storage | null {
  try { return typeof localStorage === 'undefined' ? null : localStorage; } catch { return null; }
}
