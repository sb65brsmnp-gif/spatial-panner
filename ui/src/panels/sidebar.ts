// Right-hand panel: Layers, Path & listener, Room, Output.
import type { Store } from '../model/store';
import { defaultLayer, defaultStereo, isStereo, LAYOUTS, MATERIALS, WALLS, type HeadMode, type LayerDoc, type SceneDoc } from '../model/scene';
import { getPoint, isHandle, deletePoint, movePoint } from '../model/geometry';
import type { Backend, BounceEvent, EngineInfo, OutputMode } from '../bridge/backend';
import type { Interaction } from '../view/interaction';
import { checkbox, el, fmtTime, numberInput, row, section, select, slider, vec3Inputs } from './dom';

type Tab = 'layers' | 'path' | 'room' | 'output';

export class Sidebar {
  readonly root: HTMLElement;
  private body: HTMLElement;
  private tab: Tab = 'layers';
  private tabs = new Map<Tab, HTMLElement>();
  private sig = '';
  private deferred = false;
  private pointerInside = false;
  info: EngineInfo | null = null;
  private meterEls: HTMLElement[] = [];
  // Bounce settings live here, not in the panel, because the Output tab is
  // rebuilt whenever the engine info changes (every couple of seconds).
  private bounceFrom = 0;
  private bounceTo: number | null = null;  // null: the scene's length
  private bounceRate: number | null = null;  // null: the device's rate
  private bounceState: BounceEvent | null = null;
  private bounceBar: HTMLElement | null = null;

  constructor(private store: Store, private backend: Backend, private tools: Interaction) {
    this.root = el('div', { id: 'sidebar' });
    const tabBar = el('div', { class: 'tabs' });
    const names: [Tab, string][] = [['layers', 'Layers'], ['path', 'Path & head'], ['room', 'Room'], ['output', 'Output']];
    for (const [t, label] of names) {
      const b = el('button', { class: 'tab', 'data-tab': t }, label);
      b.addEventListener('click', () => this.setTab(t));
      this.tabs.set(t, b);
      tabBar.append(b);
    }
    this.body = el('div', { class: 'sidebar-body' });
    this.root.append(tabBar, this.body);

    // Don't rebuild under the user's hands: wait until the slider/field is released.
    this.root.addEventListener('pointerdown', () => { this.pointerInside = true; });
    window.addEventListener('pointerup', () => { this.pointerInside = false; if (this.deferred) this.render(true); });
    this.root.addEventListener('focusout', () => setTimeout(() => { if (this.deferred && !this.busy()) this.render(true); }, 0));
    window.addEventListener('pointerdown', (e) => {
      const a = document.activeElement as HTMLElement | null;
      if (a && this.root.contains(a) && !this.root.contains(e.target as Node)) a.blur();
    }, { capture: true });

    store.subscribe((kinds) => {
      if (kinds.has('meters')) this.updateMeters();
      if (kinds.has('scene') || kinds.has('selection') || kinds.has('analysis') || kinds.has('file') || kinds.has('tool')) this.render();
    });
    this.setTab('layers');
    backend.onBounce((e) => this.onBounce(e));
  }

  private onBounce(e: BounceEvent): void {
    this.bounceState = e;
    // Progress only moves the bar; start and finish rebuild the section.
    if (!e.done && this.bounceBar && (e.progress ?? 0) > 0) { this.bounceBar.style.width = `${Math.round((e.progress ?? 0) * 100)}%`; return; }
    if (this.tab === 'output') this.render(true);
  }

  setTab(t: Tab): void {
    this.tab = t;
    for (const [k, b] of this.tabs) b.classList.toggle('active', k === t);
    this.render(true);
  }

  private busy(): boolean {
    const a = document.activeElement;
    return this.pointerInside || (!!a && this.root.contains(a) && a !== document.body);
  }

  render(force = false): void {
    const sig = this.signature();
    if (!force && sig === this.sig) return;
    if (!force && this.busy()) { this.deferred = true; return; }
    this.deferred = false;
    this.sig = sig;
    const scroll = this.body.scrollTop;
    this.body.replaceChildren();
    this.meterEls = [];
    switch (this.tab) {
      case 'layers': this.renderLayers(); break;
      case 'path': this.renderPath(); break;
      case 'room': this.renderRoom(); break;
      case 'output': this.renderOutput(); break;
    }
    this.body.scrollTop = scroll;
  }

  private signature(): string {
    const s = this.store.scene;
    const sel = JSON.stringify(this.store.selection);
    const a = this.store.analysis;
    const base = `${this.tab}|${sel}|${this.store.filePath}`;
    switch (this.tab) {
      case 'layers': return base + JSON.stringify(s.layers) + JSON.stringify([...this.store.audioInfo.keys()])
        + (this.plugin ? JSON.stringify(this.info?.tracks ?? []) : '');
      case 'path': return base + JSON.stringify(s.listener) + JSON.stringify(s.editor) + JSON.stringify(this.tools.opts)
        + (a ? `${a.arrival_time}|${a.paths.map((p) => p.length).join(',')}` : '') + s.layers.map((l) => l.name).join('|');
      case 'room': return base + JSON.stringify(s.room) + JSON.stringify(s.environment);
      case 'output': return base + JSON.stringify(s.editor) + JSON.stringify(this.info);
    }
  }

  private upd(fn: (s: SceneDoc) => void, key?: string): void { this.store.update(fn, key); }

  // Plugin: tracks play the layers, the host owns the transport and output.
  private get plugin(): boolean { return this.backend.kind === 'plugin'; }

  private trackName(id: string | undefined): string | null {
    if (!id) return null;
    return this.info?.tracks?.find((t) => t.id === id)?.name ?? null;
  }

  // -------------------------------------------------------------- layers

  private renderLayers(): void {
    const s = this.store.scene;
    if (this.plugin) {
      this.body.append(el('p', { class: 'hint' }, s.layers.length
        ? 'Each track running Spatial Panner plays its layer. Drag layers in the 3D view to place them.'
        : 'Insert Spatial Panner on a track to add it here as a layer. Each track plays its own layer.'));
      if (!s.layers.length) return;
    } else {
      const add = el('button', { class: 'btn primary' }, 'Add audio files…');
      add.addEventListener('click', () => this.addAudioFiles());
      const addEmpty = el('button', { class: 'btn' }, 'Add empty layer');
      addEmpty.addEventListener('click', () => this.addLayers([{ path: '', name: '' }]));
      this.body.append(el('div', { class: 'btn-row' }, add, addEmpty));
      if (!s.layers.length) {
        this.body.append(el('p', { class: 'hint' }, 'Add audio files to place them in the scene. Each file becomes a layer you can drag in the 3D view.'));
        return;
      }
    }
    const list = el('div', { class: 'layer-list' });
    const sel = this.store.selection;
    s.layers.forEach((l, i) => {
      const item = el('div', { class: 'layer-item' + (sel.kind === 'layer' && sel.index === i ? ' selected' : '') });
      const color = el('input', { type: 'color', class: 'swatch' });
      color.value = l.color ?? '#4f9cf9';
      color.addEventListener('input', () => this.upd((sc) => { sc.layers[i].color = color.value; }, `color-${i}`));
      const unbound = this.plugin && !this.trackName(l.host_id);
      const name = el('span', { class: 'layer-name' + (unbound ? ' muted' : ''),
        title: this.plugin ? (unbound ? 'No track plays this layer' : `Track: ${this.trackName(l.host_id)}`) : l.audio || 'no audio file' },
        l.name || `Layer ${i + 1}`);
      const mute = el('button', { class: 'ms' + (l.mute ? ' on mute' : ''), title: 'Mute' }, 'M');
      mute.addEventListener('click', (e) => { e.stopPropagation(); this.upd((sc) => { sc.layers[i].mute = !sc.layers[i].mute; }); });
      const solo = el('button', { class: 'ms' + (l.solo ? ' on solo' : ''), title: 'Solo' }, 'S');
      solo.addEventListener('click', (e) => { e.stopPropagation(); this.upd((sc) => { sc.layers[i].solo = !sc.layers[i].solo; }); });
      const level = slider(l.level_db, -60, 12, 0.5, (v) => this.upd((sc) => { sc.layers[i].level_db = v; }, `level-${i}`),
        (v) => `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`, () => this.store.endGesture());
      level.classList.add('level');
      const meter = el('div', { class: 'meter' }, el('div'));
      this.meterEls[i] = meter.firstChild as HTMLElement;
      const missing = l.audio && this.store.audioInfo.get(l.audio)?.error;
      item.append(el('div', { class: 'layer-top' }, color, name, missing ? el('span', { class: 'warn', title: missing }, '⚠') : '', mute, solo),
        el('div', { class: 'layer-bottom' }, level, meter));
      item.addEventListener('click', () => this.store.select({ kind: 'layer', index: i }));
      list.append(item);
    });
    this.body.append(list);
    this.updateMeters();
    if (sel.kind === 'layer' && s.layers[sel.index]) this.renderLayerDetails(sel.index);
    else this.body.append(el('p', { class: 'hint' }, 'Select a layer (here or in the 3D view) to edit it. Drag layers in the view; Shift+drag changes height.'));
  }

  private renderLayerDetails(i: number): void {
    const l = this.store.scene.layers[i];
    const u = (fn: (l: LayerDoc) => void, key?: string) => this.upd((s) => fn(s.layers[i]), key ? `${key}-${i}` : undefined);
    const nameIn = el('input', { type: 'text', class: 'text' });
    nameIn.value = l.name;
    nameIn.addEventListener('change', () => u((x) => { x.name = nameIn.value; }));
    const replace = el('button', { class: 'btn small' }, 'Replace…');
    replace.addEventListener('click', async () => {
      const files = await this.backend.chooseAudioFiles();
      if (!files.length) return;
      for (const f of files) this.store.audioInfo.set(f.path, f);
      u((x) => {
        x.audio = files[0].path;
        if (!x.name) x.name = files[0].name;
        // A stereo file plays as a pair, a mono file as one source.
        x.channels = Math.min(2, Math.max(1, files[0].channels || 1));
        if (x.channels === 2 && !x.stereo) x.stereo = defaultStereo();
      });
    });
    const info = this.store.audioInfo.get(l.audio);
    const audioDesc = l.audio ? `${l.audio.split(/[\\/]/).pop()}${info && !info.error ? ` · ${fmtTime(info.duration, false)} · ${info.channels === 1 ? 'mono' : info.channels === 2 ? 'stereo' : `${info.channels} ch`}` : ''}` : 'none';
    const stereo = isStereo(l);
    const st = l.stereo ?? defaultStereo();
    // How the file's channels are played: a left/right pair or summed to one source.
    const playAs = select(['2', '1'], stereo ? '2' : '1', (v) => u((x) => {
      x.channels = v === '2' ? 2 : 1;
      if (x.channels === 2 && !x.stereo) x.stereo = defaultStereo();
    }), { '2': 'stereo pair (left and right)', '1': 'mono (channels summed)' });
    const facing = Math.round(Math.atan2(-l.directivity_forward[0], -l.directivity_forward[2]) * 180 / Math.PI);
    const remove = el('button', { class: 'btn danger' }, 'Remove layer');
    remove.addEventListener('click', () => { this.store.select({ kind: 'layer', index: i }); this.tools.deleteSelection(); });
    const bound = this.plugin && !!this.trackName(l.host_id);
    if (this.plugin) {
      // The track plays the audio and its name names the layer.
      const tracks = this.info?.tracks ?? [];
      const ids = ['', ...tracks.map((t) => t.id)];
      if (l.host_id && !tracks.some((t) => t.id === l.host_id)) ids.push(l.host_id);
      const labels: Record<string, string> = { '': 'none' };
      for (const id of ids.slice(1)) labels[id] = this.trackName(id) ?? 'a track that is not running';
      const track = select(ids, l.host_id ?? '', (v) => this.upd((s) => {
        for (const x of s.layers) if (v && x.host_id === v) delete x.host_id;
        if (v) s.layers[i].host_id = v; else delete s.layers[i].host_id;
        const n = this.trackName(v);
        if (n) s.layers[i].name = n;
      }), labels);
      nameIn.disabled = bound;
      if (bound) nameIn.title = 'Follows the track name in Logic';
      this.body.append(section('Layer',
        row('Track', track),
        row('Name', nameIn),
        row('Track is', el('span', { class: 'muted' }, stereo ? 'stereo: the layer is a left/right pair' : 'mono: the layer is one source')),
        row(stereo ? 'Centre' : 'Position', vec3Inputs(l.position, (v) => u((x) => { x.position = v; }))),
        row('Level', numberInput(l.level_db, (v) => u((x) => { x.level_db = v; }), { step: 0.5, width: 64 }), 'dB'),
        el('p', { class: 'muted' }, 'The track\'s Level and Position offset parameters adjust this live and can be automated in Logic.'),
      ));
    } else {
      this.body.append(section('Layer',
        row('Name', nameIn),
        row('Audio', el('span', { class: 'file', title: l.audio }, audioDesc), replace),
        info && !info.error && info.channels >= 2 ? row('Play as', playAs) : '',
        row(stereo ? 'Centre' : 'Position', vec3Inputs(l.position, (v) => u((x) => { x.position = v; }))),
        row('Level', numberInput(l.level_db, (v) => u((x) => { x.level_db = v; }), { step: 0.5, width: 64 }), 'dB'),
        row('Starts at', numberInput(l.start_time, (v) => u((x) => { x.start_time = Math.max(0, v); }), { step: 0.1, width: 64 }), 's',
          checkbox(l.loop, (v) => u((x) => { x.loop = v; }), 'loop')),
      ));
    }
    if (stereo) {
      const us = (fn: (s: typeof st) => void, key?: string) => u((x) => { x.stereo = x.stereo ?? defaultStereo(); fn(x.stereo); }, key);
      this.body.append(section('Stereo field',
        el('p', { class: 'muted' }, 'Left and right play from the two ends of the bar. Drag the bar to move the pair, drag an end to widen, narrow or turn it; Option-drag an end keeps the centre fixed.'),
        row('Width', slider(st.width, 0, 12, 0.05, (v) => us((x) => { x.width = v; }, 'stwidth'), (v) => `${v.toFixed(2)} m`, () => this.store.endGesture())),
        row('Rotation', slider(st.rotation, -180, 180, 1, (v) => us((x) => { x.rotation = v; }, 'strot'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
        row('Elevation', slider(st.elevation, -90, 90, 1, (v) => us((x) => { x.elevation = v; }, 'stel'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
        row('Mono', checkbox(st.mono, (v) => us((x) => { x.mono = v; }), 'sum left and right at the centre')),
        this.plugin ? el('p', { class: 'muted' }, 'The track\'s Stereo Width, Stereo Rotation and Mono parameters adjust this live and can be automated in Logic.') : '',
      ));
    }
    this.body.append(section('Sound source',
      row('Doppler', slider(l.doppler * 100, 0, 100, 1, (v) => u((x) => { x.doppler = v / 100; }, 'doppler'), (v) => `${v.toFixed(0)} %`, () => this.store.endGesture())),
      row('Spread', slider(l.spread_deg, 0, 180, 1, (v) => u((x) => { x.spread_deg = v; }, 'spread'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
      row('Directivity', slider(l.directivity, 0, 1, 0.01, (v) => u((x) => { x.directivity = v; }, 'dir'), (v) => v < 0.01 ? 'omni' : v > 0.99 ? 'cardioid' : v.toFixed(2), () => this.store.endGesture())),
      row('Facing', slider(facing, -180, 180, 1, (v) => u((x) => {
        const r = v * Math.PI / 180;
        x.directivity_forward = [Math.round(-Math.sin(r) * 1000) / 1000, 0, Math.round(-Math.cos(r) * 1000) / 1000];
      }, 'facing'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
    ));
    this.body.append(section('Distance',
      row('Reference', numberInput(l.reference_distance, (v) => u((x) => { x.reference_distance = Math.max(0.05, v); }), { step: 0.1, width: 64 }), 'm'),
      row('Closest', numberInput(l.min_distance, (v) => u((x) => { x.min_distance = Math.max(0.01, v); }), { step: 0.05, width: 64 }), 'm'),
      row('Rolloff', numberInput(l.rolloff, (v) => u((x) => { x.rolloff = Math.max(0, v); }), { step: 0.1, width: 64 }), '(1 = −6 dB per doubling)'),
      row('Room send', numberInput(l.reverb_send_db, (v) => u((x) => { x.reverb_send_db = v; }), { step: 0.5, width: 64 }), 'dB'),
      row('Reflections', select(['-1', '0', '1', '2', '3'], String(l.reflection_order), (v) => u((x) => { x.reflection_order = parseInt(v, 10); }),
        { '-1': 'room default', '0': 'none', '1': '1st order', '2': '2nd order', '3': '3rd order' })),
    ), bound ? el('p', { class: 'muted' }, 'To remove this layer, remove Spatial Panner from its track.') : el('div', { class: 'btn-row' }, remove));
  }

  async addAudioFiles(): Promise<void> {
    const files = await this.backend.chooseAudioFiles();
    if (!files.length) return;
    for (const f of files) this.store.audioInfo.set(f.path, f);
    this.addLayers(files);
  }

  // New layers go on a ring around the listener's start, facing it. Stereo
  // files become left/right pairs.
  private addLayers(files: { path: string; name: string; channels?: number }[]): void {
    const L = this.store.scene.listener;
    const first = L.paths[L.active_path]?.segments[0]?.points[0] ?? L.static_position;
    this.upd((s) => {
      const n0 = s.layers.length;
      files.forEach((f, k) => {
        const i = n0 + k;
        const a = (i * 2.399963) % (Math.PI * 2);  // golden angle: spreads any number of layers evenly
        const r = 3 + 0.4 * Math.floor(i / 6);
        const pos: [number, number, number] = [
          Math.round((first[0] - r * Math.sin(a)) * 100) / 100, 1.6, Math.round((first[2] - r * Math.cos(a)) * 100) / 100];
        const channels = Math.min(2, Math.max(1, f.channels || 1));
        s.layers.push(defaultLayer(i, { name: f.name || `Layer ${i + 1}`, audio: f.path, position: pos, channels,
          ...(channels === 2 ? { stereo: defaultStereo() } : {}) }));
      });
    });
    this.store.select({ kind: 'layer', index: this.store.scene.layers.length - 1 });
  }

  updateMeters(): void {
    const m = this.store.meters;
    this.meterEls.forEach((e, i) => {
      if (!e) return;
      const db = m[i] ?? -120;
      e.style.width = `${Math.max(0, Math.min(1, (db + 60) / 60)) * 100}%`;
      e.style.background = db > -3 ? '#eb5757' : db > -12 ? '#f2c94c' : '#5fd38d';
    });
  }

  // ---------------------------------------------------------------- path

  private renderPath(): void {
    const s = this.store.scene;
    const L = s.listener;
    const o = this.tools.opts;
    const a = this.store.analysis;
    const ed = s.editor ?? {};

    this.body.append(section('Drawing',
      row('Height', numberInput(ed.draw_height ?? 1.7, (v) => this.upd((sc) => { sc.editor = { ...sc.editor, draw_height: v }; }), { step: 0.1, width: 64 }), 'm (ear height)'),
      row('Snap', select(['0', '0.1', '0.25', '0.5', '1'], String(o.grid), (v) => { o.grid = parseFloat(v); this.render(true); },
        { '0': 'off', '0.1': '10 cm', '0.25': '25 cm', '0.5': '50 cm', '1': '1 m' })),
      row('', checkbox(o.append, (v) => { o.append = v; this.render(true); }, 'New strokes continue the active path')),
      row('Spiral', numberInput(o.spiralTurns, (v) => { o.spiralTurns = Math.max(0.25, v); }, { step: 0.5, width: 56 }), 'turns'),
      row('Helix', numberInput(o.helixTurns, (v) => { o.helixTurns = Math.max(0.25, v); }, { step: 0.5, width: 56 }), 'turns,',
        numberInput(o.helixRise, (v) => { o.helixRise = v; }, { step: 0.5, width: 56 }), 'm rise'),
    ));

    const list = el('div', { class: 'path-list' });
    if (!L.paths.length) list.append(el('p', { class: 'hint' }, 'No path yet: pick a drawing tool in the toolbar and draw in the 3D view. Without a path the listener stands still.'));
    L.paths.forEach((p, i) => {
      const item = el('div', { class: 'path-item' + (i === L.active_path ? ' active' : '') });
      const radio = el('input', { type: 'radio', name: 'active-path' });
      radio.checked = i === L.active_path;
      radio.title = 'The listener follows this path';
      radio.addEventListener('change', () => this.upd((sc) => { sc.listener.active_path = i; }));
      const name = el('input', { type: 'text', class: 'text' });
      name.value = p.name;
      name.addEventListener('change', () => this.upd((sc) => { sc.listener.paths[i].name = name.value; }));
      const len = a?.paths[i]?.length;
      const closed = checkbox(p.closed, (v) => this.upd((sc) => { sc.listener.paths[i].closed = v; }), 'closed');
      const dup = el('button', { class: 'btn small', title: 'Duplicate' }, '⧉');
      dup.addEventListener('click', () => this.upd((sc) => {
        const c = JSON.parse(JSON.stringify(sc.listener.paths[i]));
        c.name = `${c.name} copy`;
        sc.listener.paths.push(c);
        sc.listener.active_path = sc.listener.paths.length - 1;
      }));
      const del = el('button', { class: 'btn small danger', title: 'Delete path' }, '✕');
      del.addEventListener('click', () => this.upd((sc) => {
        sc.listener.paths.splice(i, 1);
        sc.listener.active_path = Math.max(0, Math.min(sc.listener.active_path, sc.listener.paths.length - 1));
      }));
      item.append(radio, name, el('span', { class: 'muted' }, len !== undefined ? `${len.toFixed(1)} m` : ''), closed, dup, del);
      list.append(item);
    });
    const travel = a && a.arrival_time > 0 ? `Reaches the end at ${fmtTime(a.arrival_time, true)}` : L.paths.length ? 'Does not reach the end within the scene' : '';
    this.body.append(section('Paths', list, travel ? el('p', { class: 'muted' }, travel) : ''));

    const sel = this.store.selection;
    if (sel.kind === 'point') {
      const p = L.paths[sel.path];
      const pos = p && getPoint(p, sel.ref);
      if (pos) {
        const handle = isHandle(p.segments[sel.ref.seg], sel.ref.pt);
        const del = el('button', { class: 'btn small danger' }, 'Delete point');
        del.addEventListener('click', () => this.upd((sc) => { deletePoint(sc.listener.paths[sel.path], sel.ref); }));
        this.body.append(section(handle ? 'Curve handle' : 'Path point',
          row('Position', vec3Inputs(pos, (v) => this.upd((sc) => { movePoint(sc.listener.paths[sel.path], sel.ref, v); }))),
          handle ? '' : el('div', { class: 'btn-row' }, del)));
      }
    } else if (L.paths.length) {
      this.body.append(el('p', { class: 'hint' }, 'Drag points to reshape the path (Shift+drag: height; front/side views edit height directly). Alt+click the path to add a point, Delete removes the selected one.'));
    }

    // Listener movement.
    const startPos = row('Start', numberInput(L.path_start_time, (v) => this.upd((sc) => { sc.listener.path_start_time = Math.max(0, v); }), { step: 0.5, width: 64 }), 's',
      checkbox(L.loop_path, (v) => this.upd((sc) => { sc.listener.loop_path = v; }), 'loop the path'));
    const fraction = row('Position', slider(L.path_fraction * 100, 0, 100, 0.1, (v) => this.upd((sc) => { sc.listener.path_fraction = v / 100; }, 'fraction'),
      (v) => `${v.toFixed(1)} %`, () => this.store.endGesture()));
    this.body.append(section('Listener movement',
      row('Moves by', select(['speed', 'along_path'], L.position_mode, (v) => this.upd((sc) => { sc.listener.position_mode = v as 'speed' | 'along_path'; }),
        { speed: 'speed curve (timeline)', along_path: 'position along path' })),
      L.position_mode === 'speed' ? startPos : fraction,
      L.paths.length ? '' : row('Stands at', vec3Inputs(L.static_position, (v) => this.upd((sc) => { sc.listener.static_position = v; }))),
    ));

    // Head.
    const H = L.head;
    const layerNames = s.layers.map((l, i) => l.name || `Layer ${i + 1}`);
    const lookAt = H.mode === 'look_at' ? [
      row('Target', select(['-1', ...layerNames.map((_, i) => String(i))], String(H.look_at_layer),
        (v) => this.upd((sc) => { sc.listener.head.look_at_layer = parseInt(v, 10); }),
        Object.fromEntries([['-1', 'a point'], ...layerNames.map((n, i) => [String(i), n])]))),
      H.look_at_layer < 0 ? row('Point', vec3Inputs(H.look_at_point, (v) => this.upd((sc) => { sc.listener.head.look_at_point = v; }))) : '',
    ] : [];
    this.body.append(section('Head',
      row('Faces', select(['along_path', 'look_at', 'keyframed'], H.mode, (v) => this.upd((sc) => { sc.listener.head.mode = v as HeadMode; }),
        { along_path: 'where it walks', look_at: 'a target', keyframed: 'keyframes only' })),
      ...lookAt,
      el('p', { class: 'muted' }, H.mode === 'keyframed'
        ? 'Yaw and pitch keys on the timeline set the head direction.'
        : 'Yaw and pitch keys on the timeline turn the head relative to this.'),
      row('Turn', slider(H.yaw_offset, -180, 180, 1, (v) => this.upd((sc) => { sc.listener.head.yaw_offset = v; }, 'yaw-off'),
        (v) => `${v.toFixed(0)}° ${v > 0 ? 'left' : v < 0 ? 'right' : ''}`, () => this.store.endGesture())),
      row('Tilt', slider(H.pitch_offset, -90, 90, 1, (v) => this.upd((sc) => { sc.listener.head.pitch_offset = v; }, 'pitch-off'),
        (v) => `${v.toFixed(0)}° ${v > 0 ? 'up' : v < 0 ? 'down' : ''}`, () => this.store.endGesture())),
    ));
  }

  // ---------------------------------------------------------------- room

  private renderRoom(): void {
    const s = this.store.scene;
    const R = s.room;
    const E = s.environment;
    const box = R.type === 'box';
    const mesh = R.type === 'mesh';
    const types = mesh || R.mesh ? ['box', 'mesh', 'outdoor', 'none'] : ['box', 'outdoor', 'none'];
    const mats = WALLS.filter((w) => box || w === 'floor').map((w) =>
      row(box ? w[0].toUpperCase() + w.slice(1) : 'Ground', select(MATERIALS, R.materials[w].name, (v) => this.upd((sc) => { sc.room.materials[w] = { name: v }; }),
        Object.fromEntries(MATERIALS.map((m) => [m, m.replace('_', ' ')])))));
    this.body.append(section('Space',
      row('Type', select(types, R.type, (v) => this.upd((sc) => { sc.room.type = v as SceneDoc['room']['type']; }),
        { box: 'room (box)', mesh: 'room (mesh)', outdoor: 'outdoors (ground only)', none: 'free field (no reflections)' })),
      mesh ? el('p', { class: 'muted' }, meshNote(R.mesh, this.info?.steamAudio)) : '',
      box ? row('Size', vec3Inputs(R.size, (v) => this.upd((sc) => { sc.room.size = v.map((x) => Math.max(1, x)) as typeof v; }), 0.5)) : '',
      box ? el('p', { class: 'muted' }, 'Width (x), height (y), depth (z) in metres.') : '',
      box ? row('Centre', vec3Inputs(R.origin, (v) => this.upd((sc) => { sc.room.origin = v; }), 0.5)) : '',
    ));
    if (R.type !== 'none' && !mesh) this.body.append(section(box ? 'Surfaces' : 'Ground', ...mats));
    if (R.objects?.length) {
      this.body.append(section('Objects',
        ...R.objects.map((o) => row(o.name || 'object', el('span', { class: 'muted' }, `${o.material.name}, ${o.max.map((v, i) => (v - o.min[i]).toFixed(1)).join(' × ')} m`))),
        el('p', { class: 'muted' }, 'Walls and objects block and reflect sound when the ray-traced room model is in use. They are set in the scene file for now.')));
    }
    this.body.append(section('Reflections and reverb',
      row('Reflections', checkbox(R.reflections, (v) => this.upd((sc) => { sc.room.reflections = v; })),
        select(['0', '1', '2', '3'], String(R.reflection_order), (v) => this.upd((sc) => { sc.room.reflection_order = parseInt(v, 10); }),
          { '0': 'none', '1': '1st order', '2': '2nd order', '3': '3rd order' })),
      row('Refl. level', numberInput(R.reflections_level_db, (v) => this.upd((sc) => { sc.room.reflections_level_db = v; }), { step: 0.5, width: 64 }), 'dB'),
      box ? row('Reverb', checkbox(R.reverb, (v) => this.upd((sc) => { sc.room.reverb = v; })),
        numberInput(R.reverb_level_db, (v) => this.upd((sc) => { sc.room.reverb_level_db = v; }), { step: 0.5, width: 64 }), 'dB') : '',
      box ? row('Decay ×', numberInput(R.reverb_time_scale, (v) => this.upd((sc) => { sc.room.reverb_time_scale = Math.max(0.1, v); }), { step: 0.1, width: 64 })) : '',
      el('p', { class: 'muted' }, 'Room changes restart the room model, so you may hear a short fade.'),
    ));
    this.body.append(section('Air',
      row('Temperature', numberInput(E.temperature_c, (v) => this.upd((sc) => { sc.environment.temperature_c = v; }), { step: 1, width: 64 }), '°C'),
      row('Humidity', numberInput(E.humidity, (v) => this.upd((sc) => { sc.environment.humidity = Math.max(0, Math.min(100, v)); }), { step: 5, width: 64 }), '%'),
      row('', checkbox(E.air_absorption, (v) => this.upd((sc) => { sc.environment.air_absorption = v; }), 'High-frequency loss over distance')),
      row('Speed of sound', numberInput(E.speed_of_sound, (v) => this.upd((sc) => { sc.environment.speed_of_sound = Math.max(50, v); }), { step: 1, width: 64 }), 'm/s'),
    ));
  }

  // -------------------------------------------------------------- output

  private renderOutput(): void {
    if (this.plugin) { this.renderPluginOutput(); return; }
    const out = this.store.scene.editor?.output ?? { mode: 'binaural' as OutputMode, layout: '7.1.4' };
    const setOut = (mode: OutputMode, layout: string) => {
      this.upd((sc) => { sc.editor = { ...sc.editor, output: { mode, layout } }; });
      this.backend.setOutput({ mode, layout }).then((r) => { if (!r.ok && r.error) this.flash(r.error); });
    };
    const info = this.info;
    const settings = el('button', { class: 'btn' }, 'Audio device…');
    settings.addEventListener('click', () => this.backend.showAudioSettings());
    const needed = out.mode === 'binaural' ? 2 : out.mode === 'ambix' ? 16 : layoutChannels(out.layout);
    this.body.append(section('Listen through',
      row('Output', select(['binaural', 'speakers', 'ambix'], out.mode, (v) => setOut(v as OutputMode, out.layout),
        { binaural: 'Headphones (binaural)', speakers: 'Speakers', ambix: 'Ambisonics (ambiX, 3rd order)' })),
      out.mode === 'speakers' ? row('Layout', select(LAYOUTS, out.layout, (v) => setOut('speakers', v))) : '',
      el('p', { class: 'muted' }, out.mode === 'binaural' ? 'Uses the SADIE II KU100 head (HRTF). Wear headphones.'
        : `Needs ${needed} output channels, sent to outputs 1–${needed} in ${out.mode === 'ambix' ? 'ACN/SN3D order' : 'the layout\'s order'}.`),
      info && info.outputChannels < needed ? el('p', { class: 'warn-text' }, `The current device has ${info.outputChannels} outputs; only the first ${info.outputChannels} channels will play.`) : '',
      el('div', { class: 'btn-row' }, settings),
      info ? el('p', { class: 'muted' }, `${info.device} · ${(info.sampleRate / 1000).toFixed(1)} kHz · ${info.outputChannels} outputs · ${info.status}`) : '',
    ));

    const end = this.bounceTo ?? this.store.duration;
    const rate = this.bounceRate ?? info?.sampleRate ?? 48000;
    const st = this.bounceState;
    const running = !!st && !st.done;
    const bounce = el('button', { class: 'btn primary' }, running ? 'Bouncing…' : 'Bounce to WAV…') as HTMLButtonElement;
    bounce.disabled = running;
    bounce.addEventListener('click', async () => {
      try {
        await this.backend.bounce({ mode: out.mode, layout: out.layout, start: this.bounceFrom, end, sampleRate: rate });
      } catch (e) {
        this.flash(String(e instanceof Error ? e.message : e), 'error');
      }
    });
    const name = (p: string) => p.split(/[\\/]/).pop() ?? p;
    let status: HTMLElement | string = '';
    this.bounceBar = null;
    if (running) {
      this.bounceBar = el('div', { class: 'progress-fill' });
      this.bounceBar.style.width = `${Math.round((st.progress ?? 0) * 100)}%`;
      status = el('div', {}, el('p', { class: 'muted' }, `Rendering ${name(st.path)}…`), el('div', { class: 'progress' }, this.bounceBar));
    } else if (st?.error) {
      status = el('p', { class: 'warn-text' }, `Bounce failed: ${st.error}`);
    } else if (st) {
      const show = el('button', { class: 'btn' }, 'Show in Finder');
      show.addEventListener('click', () => { void this.backend.revealFile(st.path); });
      status = el('div', {}, el('p', { class: 'muted' }, `Saved ${st.path}`), el('div', { class: 'btn-row' }, show));
    }
    this.body.append(section('Bounce',
      row('From', numberInput(this.bounceFrom, (v) => { this.bounceFrom = Math.max(0, v); }, { step: 1, width: 64 }), 's  to',
        numberInput(end, (v) => { this.bounceTo = Math.max(0, v); }, { step: 1, width: 64 }), 's'),
      row('Sample rate', select(['44100', '48000', '88200', '96000'], String(rate), (v) => { this.bounceRate = parseInt(v, 10); })),
      el('p', { class: 'muted' }, 'Renders offline with the output setting above: binaural stereo, the speaker layout, or ambiX.'),
      el('div', { class: 'btn-row' }, bounce),
      status,
    ));
  }

  private renderPluginOutput(): void {
    const info = this.info;
    this.body.append(section('Listen through',
      el('p', { class: 'muted' }, 'Each track sets its own output. On a stereo track Spatial Panner renders binaural for headphones '
        + '(or plain stereo speakers, chosen in the plugin header); on a surround track it renders that track\'s speaker layout, up to 7.1.4.'),
      el('p', { class: 'muted' }, 'To bounce, use File > Bounce in Logic: every track renders its own layer and the mix sums them.'),
      info ? el('p', { class: 'muted' }, `${info.device} · ${(info.sampleRate / 1000).toFixed(1)} kHz · ${info.status}`) : '',
    ));
  }

  private flash(text: string, level: 'info' | 'error' = 'info'): void {
    window.dispatchEvent(new CustomEvent('sp-message', { detail: { text, level } }));
  }
}

function layoutChannels(name: string): number {
  return ({ stereo: 2, quad: 4, '5.1': 6, '7.1': 8, '5.1.4': 10, '7.1.4': 12, '9.1.6': 16 } as Record<string, number>)[name] ?? 2;
}

function meshNote(mesh: unknown, steam: boolean | undefined): string {
  const file = typeof mesh === 'object' && mesh && 'file' in mesh ? String((mesh as { file: unknown }).file).split(/[\\/]/).pop() : null;
  const from = file ? `Shape from ${file}.` : 'Shape stored in the scene file.';
  return steam === false ? `${from} This build has no ray tracer, so a mesh room plays as free field.` : `${from} Surfaces come from the mesh's materials.`;
}
