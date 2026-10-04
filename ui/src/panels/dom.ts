// Small DOM helpers for the panels (no framework).

export function el<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Record<string, string> = {}, ...children: (Node | string)[]): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  for (const c of children) e.append(c);
  return e;
}

export function fmtTime(t: number, tenths: boolean): string {
  const neg = t < 0;
  t = Math.abs(t);
  const m = Math.floor(t / 60);
  const s = t - m * 60;
  const ss = tenths ? s.toFixed(1).padStart(4, '0') : Math.floor(s).toString().padStart(2, '0');
  return `${neg ? '-' : ''}${m}:${ss}`;
}

export interface NumOpts { step?: number; min?: number; max?: number; width?: number; decimals?: number }

// A number field that commits on change (Enter / blur) and on arrow keys.
export function numberInput(value: number, onChange: (v: number) => void, o: NumOpts = {}): HTMLInputElement {
  const i = el('input', { type: 'number', class: 'num' });
  i.step = String(o.step ?? 0.1);
  if (o.min !== undefined) i.min = String(o.min);
  if (o.max !== undefined) i.max = String(o.max);
  if (o.width) i.style.width = `${o.width}px`;
  const fmt = (v: number) => String(Math.round(v * 1000) / 1000);
  i.value = fmt(value);
  i.addEventListener('change', () => {
    let v = parseFloat(i.value);
    if (!Number.isFinite(v)) return;
    if (o.min !== undefined) v = Math.max(o.min, v);
    if (o.max !== undefined) v = Math.min(o.max, v);
    onChange(v);
  });
  return i;
}

export function select(options: string[], value: string, onChange: (v: string) => void, labels: Record<string, string> = {}): HTMLSelectElement {
  const s = el('select');
  for (const o of options) {
    const opt = el('option', { value: o }, labels[o] ?? o);
    s.append(opt);
  }
  s.value = value;
  s.addEventListener('change', () => onChange(s.value));
  return s;
}

export function checkbox(value: boolean, onChange: (v: boolean) => void, label?: string): HTMLLabelElement {
  const c = el('input', { type: 'checkbox' });
  c.checked = value;
  c.addEventListener('change', () => onChange(c.checked));
  const l = el('label', { class: 'check' }, c);
  if (label) l.append(label);
  return l;
}

// Range slider with a value readout; `onInput` fires while dragging.
export function slider(value: number, min: number, max: number, step: number, onInput: (v: number) => void,
  fmt: (v: number) => string = (v) => v.toFixed(1), onEnd?: () => void): HTMLElement {
  const wrap = el('div', { class: 'slider' });
  const r = el('input', { type: 'range' });
  r.min = String(min); r.max = String(max); r.step = String(step); r.value = String(value);
  const out = el('span', { class: 'slider-val' }, fmt(value));
  r.addEventListener('input', () => { const v = parseFloat(r.value); out.textContent = fmt(v); onInput(v); });
  r.addEventListener('change', () => onEnd?.());
  r.addEventListener('dblclick', () => { const v = Math.min(max, Math.max(min, 0)); r.value = String(v); out.textContent = fmt(v); onInput(v); onEnd?.(); });
  wrap.append(r, out);
  return wrap;
}

export function row(label: string, ...controls: (Node | string)[]): HTMLElement {
  return el('div', { class: 'row' }, el('span', { class: 'row-label' }, label), el('div', { class: 'row-ctl' }, ...controls));
}

export function vec3Inputs(v: [number, number, number], onChange: (v: [number, number, number]) => void, step = 0.1): HTMLElement {
  const wrap = el('div', { class: 'vec3' });
  ['x', 'y', 'z'].forEach((axis, k) => {
    const i = numberInput(v[k], (n) => { const c = [...v] as [number, number, number]; c[k] = n; onChange(c); }, { step, width: 58 });
    i.title = axis;
    wrap.append(el('span', { class: 'axis' }, axis), i);
  });
  return wrap;
}

export function section(title: string, ...children: (Node | string)[]): HTMLElement {
  return el('div', { class: 'section' }, el('div', { class: 'section-title' }, title), ...children);
}
