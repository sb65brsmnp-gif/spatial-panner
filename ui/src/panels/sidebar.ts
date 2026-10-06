// Right-hand panel: Layers, Path & listener, Room, Output.
import type { Store } from '../model/store';
import { defaultLayer, defaultStereo, defaultAmbisonic, defaultScene, defaultRoom, layerHome, isStereo, isAmbisonic, ambisonicOrder, channelsForFile, layerExtras, AMBISONIC_CHANNELS,
  LAYOUTS, MATERIALS, WALLS, layerColor, hasPath, defaultLink, isSpatialized, isLinked, leadsTo, type HeadMode, type LayerDoc, type LayerTiming, type PathEnd, type SceneDoc, type V3 } from '../model/scene';
import { fitTiming, fittableLayers, layerArrival, layerOfPath, layerPathLength, layerStart, listenerPathLength, removePath } from '../model/layerMotion';
import { speedArrival } from '../model/scene';
import { getPoint, isHandle, deletePoint, movePoint } from '../model/geometry';
import type { Backend, BounceEvent, EngineInfo, OutputMode } from '../bridge/backend';
import type { Interaction } from '../view/interaction';
import { checkbox, el, fmtTime, numberInput, resetOnOptionClick, row, section, select, slider, vec3Inputs } from './dom';

type Tab = 'layers' | 'path' | 'room' | 'output';

// Defaults that Option-click on a control returns to.
const D = defaultLayer(0);
const DS = defaultStereo();
const DA = defaultAmbisonic();
const DL = defaultScene().listener;
const DR = defaultRoom();
const DE = defaultScene().environment;

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
  // Fit timing: what is left out (listener 'L', layer indices) and the span
  // (null: from the earliest start to the latest finish).
  private fitOut = new Set<string>();
  private fitStart: number | null = null;
  private fitEnd: number | null = null;

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

  // A field being typed in, or a control under the pointer, must not be
  // rebuilt. A menu or tick box keeps focus after its change, and that change
  // may need the panel rebuilt (room type: box walls or ground), so they do
  // not count.
  private busy(): boolean {
    const a = document.activeElement as HTMLElement | null;
    const typing = !!a && this.root.contains(a)
      && ((a.tagName === 'INPUT' && !['checkbox', 'range'].includes((a as HTMLInputElement).type)) || a.tagName === 'TEXTAREA');
    return this.pointerInside || typing;
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
      case 'layers': return base + JSON.stringify(s.layers) + JSON.stringify([...this.store.audioInfo.keys()]) + `|${this.tools.opts.layerTarget}|${this.tools.opts.tool}`
        + (a?.layers ?? []).map((t) => `${t.layer}:${t.length}`).join(',')
        + (this.plugin ? JSON.stringify(this.info?.tracks ?? []) : '');
      case 'path': return base + JSON.stringify(s.listener) + JSON.stringify(s.editor) + JSON.stringify(this.tools.opts)
        + (a ? `${a.arrival_time}|${a.paths.map((p) => p.length).join(',')}|${(a.layers ?? []).map((t) => t.length).join(',')}` : '') + s.layers.map((l) => l.name).join('|')
        + JSON.stringify(s.layers.map((l) => l.motion ? [l.motion.timing, l.motion.start_time, l.motion.speed, l.motion.keys] : null))
        + `|${[...this.fitOut].join(',')}|${this.fitStart}|${this.fitEnd}`;
      case 'room': return base + JSON.stringify(s.room) + JSON.stringify(s.environment)
        + (s.room.impulse_response ? JSON.stringify(this.store.audioInfo.get(s.room.impulse_response.file) ?? null) : '');
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
      const addAmbi = el('button', { class: 'btn' }, 'Add Ambisonic from separate files…');
      addAmbi.title = 'Choose the 4 (or 9, 16) mono files of one Ambisonic recording; they are taken in name order (W X Y Z for FuMa, 0 1 2 3… for ambiX).';
      addAmbi.addEventListener('click', () => this.addAmbisonicFiles());
      this.body.append(el('div', { class: 'btn-row' }, add, addEmpty), el('div', { class: 'btn-row' }, addAmbi));
      if (!s.layers.length) {
        this.body.append(el('p', { class: 'hint' }, 'Add audio files to place them in the scene. Each file becomes a layer you can drag in the 3D view. '
          + 'A 4-, 9- or 16-channel file is an Ambisonic recording: it becomes a sphere you can walk into.'));
        return;
      }
    }
    const list = el('div', { class: 'layer-list' });
    const sel = this.store.selection;
    const selLayer = sel.kind === 'layer' ? sel.index : sel.kind === 'point' ? layerOfPath(sel.path) : -1;
    s.layers.forEach((l, i) => {
      const item = el('div', { class: 'layer-item' + (selLayer === i ? ' selected' : '') });
      const color = el('input', { type: 'color', class: 'swatch' });
      color.value = l.color ?? '#4f9cf9';
      color.addEventListener('input', () => this.upd((sc) => { sc.layers[i].color = color.value; }, `color-${i}`));
      resetOnOptionClick(color, () => this.upd((sc) => { sc.layers[i].color = layerColor(i); }));
      const unbound = this.plugin && !this.trackName(l.host_id);
      const name = el('span', { class: 'layer-name' + (unbound ? ' muted' : ''),
        title: this.plugin ? (unbound ? 'No track plays this layer' : `Track: ${this.trackName(l.host_id)}`) : l.audio || 'no audio file' },
        l.name || `Layer ${i + 1}`);
      const mute = el('button', { class: 'ms' + (l.mute ? ' on mute' : ''), title: 'Mute' }, 'M');
      mute.addEventListener('click', (e) => { e.stopPropagation(); this.upd((sc) => { sc.layers[i].mute = !sc.layers[i].mute; }); });
      const solo = el('button', { class: 'ms' + (l.solo ? ' on solo' : ''), title: 'Solo' }, 'S');
      solo.addEventListener('click', (e) => { e.stopPropagation(); this.upd((sc) => { sc.layers[i].solo = !sc.layers[i].solo; }); });
      const level = slider(l.level_db, -60, 12, 0.5, (v) => this.upd((sc) => { sc.layers[i].level_db = v; }, `level-${i}`),
        (v) => `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`, () => this.store.endGesture(), D.level_db);
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
    if (selLayer >= 0 && s.layers[selLayer]) this.renderLayerDetails(selLayer);
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
        delete x.audio_files;
        if (!x.name) x.name = files[0].name;
        // A stereo file plays as a pair, a mono file as one source, a
        // 4 / 9 / 16-channel file as an Ambisonic sphere.
        x.channels = channelsForFile(files[0].channels || 1);
        Object.assign(x, layerExtras(x.channels, x));
      });
    });
    const files = l.audio_files ?? [];
    const info = files.length ? this.store.audioInfo.get(files[0]) : this.store.audioInfo.get(l.audio);
    const chDesc = (n: number) => n === 1 ? 'mono' : n === 2 ? 'stereo' : AMBISONIC_CHANNELS.includes(n) ? `${n} ch, Ambisonic order ${ambisonicOrder(n)}` : `${n} ch`;
    const audioDesc = files.length ? `${files.length} files: ${files[0].split(/[\\/]/).pop()} …${info && !info.error ? ` · ${fmtTime(info.duration, false)}` : ''}`
      : l.audio ? `${l.audio.split(/[\\/]/).pop()}${info && !info.error ? ` · ${fmtTime(info.duration, false)} · ${chDesc(info.channels)}` : ''}` : 'none';
    const stereo = isStereo(l);
    const ambi = isAmbisonic(l);
    const st = l.stereo ?? defaultStereo();
    const am = l.ambisonic ?? defaultAmbisonic();
    // How the file's channels are played: an Ambisonic sphere (when the file
    // has a full order's channels), a left/right pair, or summed to one source.
    const fileCh = info && !info.error ? info.channels : 0;
    const playOptions = [...(AMBISONIC_CHANNELS.includes(fileCh) ? [String(fileCh)] : []), ...(fileCh >= 2 ? ['2'] : []), '1'];
    const playAs = select(playOptions, String(l.channels ?? 1), (v) => u((x) => {
      x.channels = parseInt(v, 10);
      Object.assign(x, layerExtras(x.channels, x));
    }), { [String(fileCh)]: `Ambisonic sphere (order ${ambisonicOrder(fileCh)})`, '2': 'stereo pair (left and right)', '1': 'mono (channels summed)' },
    String(channelsForFile(fileCh || 1)));
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
      // An Ambisonic recording of a higher order than the track carries
      // (Logic has no 9- or 16-channel track) plays from a file instead.
      const fromFile = ambi && !!l.audio;
      const pick = el('button', { class: 'btn small' }, l.audio ? 'Replace…' : 'Choose…');
      pick.addEventListener('click', async () => {
        const f = await this.backend.chooseFile('Choose an Ambisonic recording (4, 9 or 16 channels, ambiX or FuMa)', '*.wav;*.aif;*.aiff;*.flac;*.caf');
        if (!f) return;
        const [fi] = await this.backend.audioInfo([f.path]);
        if (fi) this.store.audioInfo.set(fi.path, fi);
        if (!fi || fi.error) { this.flash(`${f.name}: ${fi?.error ?? 'cannot read'}`, 'error'); return; }
        if (!AMBISONIC_CHANNELS.includes(fi.channels)) { this.flash(`${f.name} has ${fi.channels} channels; an Ambisonic recording has 4, 9 or 16.`, 'error'); return; }
        u((x) => { x.audio = fi.path; x.channels = fi.channels; Object.assign(x, layerExtras(fi.channels, x)); });
      });
      const clearFile = el('button', { class: 'btn small' }, 'Clear');
      clearFile.addEventListener('click', () => u((x) => { x.audio = ''; }));
      this.body.append(section('Layer',
        row('Track', track),
        row('Name', nameIn),
        row('Track is', el('span', { class: 'muted' }, fromFile ? `playing a ${l.channels}-channel recording from the file below (its own audio is not used)`
          : ambi ? `${l.channels} channels: the layer is an Ambisonic sphere (order ${ambisonicOrder(l.channels ?? 4)})`
          : stereo ? 'stereo: the layer is a left/right pair' : 'mono: the layer is one source')),
        row('Recording', el('span', { class: 'file', title: l.audio }, l.audio ? audioDesc : 'none (the track plays this layer)'), pick, l.audio ? clearFile : ''),
        el('p', { class: 'muted' }, 'A first-order recording plays from a quad track in Logic. For second or third order (9 or 16 channels), choose the file here: this track then plays it in sync with the song.'),
        row(stereo || ambi ? 'Centre' : 'Position', vec3Inputs(l.position, (v) => u((x) => { x.position = v; }), 0.1, layerHome(l))),
        row('Level', numberInput(l.level_db, (v) => u((x) => { x.level_db = v; }), { step: 0.5, width: 64, def: D.level_db }), 'dB'),
        el('p', { class: 'muted' }, 'The track\'s Level and Position offset parameters adjust this live and can be automated in Logic.'),
      ));
    } else {
      this.body.append(section('Layer',
        row('Name', nameIn),
        row('Audio', el('span', { class: 'file', title: files.length ? files.join('\n') : l.audio }, audioDesc), replace),
        playOptions.length > 1 && !files.length ? row('Play as', playAs) : '',
        row(stereo || ambi ? 'Centre' : 'Position', vec3Inputs(l.position, (v) => u((x) => { x.position = v; }), 0.1, layerHome(l))),
        row('Level', numberInput(l.level_db, (v) => u((x) => { x.level_db = v; }), { step: 0.5, width: 64, def: D.level_db }), 'dB'),
        row('Starts at', numberInput(l.start_time, (v) => u((x) => { x.start_time = Math.max(0, v); }), { step: 0.1, width: 64, def: D.start_time }), 's',
          checkbox(l.loop, (v) => u((x) => { x.loop = v; }), 'loop', D.loop)),
      ));
    }
    this.renderLayerPath(i);
    if (stereo) {
      const us = (fn: (s: typeof st) => void, key?: string) => u((x) => { x.stereo = x.stereo ?? defaultStereo(); fn(x.stereo); }, key);
      this.body.append(section('Stereo field',
        el('p', { class: 'muted' }, 'Left and right play from the two ends of the bar. Drag the bar to move the pair, drag an end to widen, narrow or turn it; Option-drag an end keeps the centre fixed.'),
        row('Width', slider(st.width, 0, 12, 0.05, (v) => us((x) => { x.width = v; }, 'stwidth'), (v) => `${v.toFixed(2)} m`, () => this.store.endGesture(), DS.width)),
        row('Rotation', slider(st.rotation, -180, 180, 1, (v) => us((x) => { x.rotation = v; }, 'strot'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture(), DS.rotation)),
        row('Elevation', slider(st.elevation, -90, 90, 1, (v) => us((x) => { x.elevation = v; }, 'stel'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
        row('Mono', checkbox(st.mono, (v) => us((x) => { x.mono = v; }), 'sum left and right at the centre', DS.mono)),
        this.plugin ? el('p', { class: 'muted' }, 'The track\'s Stereo Width, Stereo Rotation and Mono parameters adjust this live and can be automated in Logic.') : '',
      ));
    }
    if (ambi) {
      const ua = (fn: (a: typeof am) => void, key?: string) => u((x) => { x.ambisonic = x.ambisonic ?? defaultAmbisonic(); fn(x.ambisonic); }, key);
      const order = ambisonicOrder(l.channels ?? 4);
      this.body.append(section('Ambisonic sphere',
        el('p', { class: 'muted' }, 'The recording plays as a sphere: its sounds sit on the surface, so walking inside brings the near side closer and the far side further off. '
          + 'At the centre it is the recording as it was. Drag the ball to move the sphere and the small cube on its surface to resize it; the arrow is the recording\'s front.'),
        files.length ? el('p', { class: 'muted' }, `Channels from ${files.length} files, in name order: ${files.map((f) => f.split(/[\\/]/).pop()).join(', ')}.`) : '',
        row('Format', select(order === 1 ? ['ambix', 'fuma'] : ['ambix'], am.format, (v) => ua((a) => { a.format = v as typeof a.format; }),
          { ambix: 'ambiX (ACN / SN3D)', fuma: 'FuMa (W X Y Z)' }, DA.format)),
        row('Radius', slider(am.radius, 0.5, 30, 0.1, (v) => ua((a) => { a.radius = v; }, 'amradius'), (v) => `${v.toFixed(1)} m`, () => this.store.endGesture(), DA.radius)),
        row('Yaw', slider(am.yaw, -180, 180, 1, (v) => ua((a) => { a.yaw = v; }, 'amyaw'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
        row('Pitch', slider(am.pitch, -90, 90, 1, (v) => ua((a) => { a.pitch = v; }, 'ampitch'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
        row('Roll', slider(am.roll, -180, 180, 1, (v) => ua((a) => { a.roll = v; }, 'amroll'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
        row('Room', checkbox(am.room_send, (v) => ua((a) => { a.room_send = v; }), 'send the recording to the room\'s reverb (it carries its own room already)', DA.room_send)),
        this.plugin ? el('p', { class: 'muted' }, 'The track\'s Sphere Radius and Sphere Rotation parameters adjust this live and can be automated in Logic.') : '',
      ));
      this.body.append(section('Sound source',
        row('Doppler', slider(l.doppler * 100, 0, 100, 1, (v) => u((x) => { x.doppler = v / 100; }, 'doppler'), (v) => `${v.toFixed(0)} %`, () => this.store.endGesture(), D.doppler * 100)),
        el('p', { class: 'muted' }, 'Outside the sphere the recording falls off with distance like a source; spread and directivity do not apply.'),
      ));
      this.body.append(section('Distance',
        row('Closest', numberInput(l.min_distance, (v) => u((x) => { x.min_distance = Math.max(0.01, v); }), { step: 0.05, width: 64, def: D.min_distance }), 'm'),
        row('Rolloff', numberInput(l.rolloff, (v) => u((x) => { x.rolloff = Math.max(0, v); }), { step: 0.1, width: 64, def: D.rolloff }), '(1 = −6 dB per doubling)'),
        row('Room send', numberInput(l.reverb_send_db, (v) => u((x) => { x.reverb_send_db = v; }), { step: 0.5, width: 64, def: D.reverb_send_db }), 'dB'),
      ), bound ? el('p', { class: 'muted' }, 'To remove this layer, remove Spatial Panner from its track.') : el('div', { class: 'btn-row' }, remove));
      return;
    }
    this.body.append(section('Sound source',
      row('Doppler', slider(l.doppler * 100, 0, 100, 1, (v) => u((x) => { x.doppler = v / 100; }, 'doppler'), (v) => `${v.toFixed(0)} %`, () => this.store.endGesture(), D.doppler * 100)),
      row('Spread', slider(l.spread_deg, 0, 180, 1, (v) => u((x) => { x.spread_deg = v; }, 'spread'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture())),
      row('Directivity', slider(l.directivity, 0, 1, 0.01, (v) => u((x) => { x.directivity = v; }, 'dir'), (v) => v < 0.01 ? 'omni' : v > 0.99 ? 'cardioid' : v.toFixed(2), () => this.store.endGesture())),
      row('Facing', slider(facing, -180, 180, 1, (v) => u((x) => {
        const r = v * Math.PI / 180;
        x.directivity_forward = [Math.round(-Math.sin(r) * 1000) / 1000, 0, Math.round(-Math.cos(r) * 1000) / 1000];
      }, 'facing'), (v) => `${v.toFixed(0)}°`, () => this.store.endGesture(), 180)),
    ));
    this.body.append(section('Distance',
      row('Reference', numberInput(l.reference_distance, (v) => u((x) => { x.reference_distance = Math.max(0.05, v); }), { step: 0.1, width: 64, def: D.reference_distance }), 'm'),
      row('Closest', numberInput(l.min_distance, (v) => u((x) => { x.min_distance = Math.max(0.01, v); }), { step: 0.05, width: 64, def: D.min_distance }), 'm'),
      row('Rolloff', numberInput(l.rolloff, (v) => u((x) => { x.rolloff = Math.max(0, v); }), { step: 0.1, width: 64, def: D.rolloff }), '(1 = −6 dB per doubling)'),
      row('Room send', numberInput(l.reverb_send_db, (v) => u((x) => { x.reverb_send_db = v; }), { step: 0.5, width: 64, def: D.reverb_send_db }), 'dB'),
      row('Reflections', select(['-1', '0', '1', '2', '3'], String(l.reflection_order), (v) => u((x) => { x.reflection_order = parseInt(v, 10); }),
        { '-1': 'room default', '0': 'none', '1': '1st order', '2': '2nd order', '3': '3rd order' }, String(D.reflection_order))),
    ), bound ? el('p', { class: 'muted' }, 'To remove this layer, remove Spatial Panner from its track.') : el('div', { class: 'btn-row' }, remove));
  }

  // Straight through or spatialised, and the layer's link to the listener
  // or another layer.
  private renderSpatializeAndLink(i: number): void {
    const s = this.store.scene;
    const l = s.layers[i];
    const u = (fn: (l: LayerDoc) => void) => this.upd((sc) => fn(sc.layers[i]));
    const on = isSpatialized(l);
    this.body.append(section('Spatialize',
      row('Spatialize', checkbox(on, (v) => u((x) => { if (v) delete x.spatialize; else x.spatialize = false; }), on ? 'on' : 'off: plays straight through', true)),
      on ? el('p', { class: 'muted' }, 'Off, the layer plays as it is: mono to both ears, stereo left to left and right to right, with no distance, direction, Doppler or room. Its level, mute, fades and place in the scene stay.')
        : row('Room', checkbox(!!l.room_send, (v) => u((x) => { if (v) x.room_send = true; else delete x.room_send; }), 'send it to the room\'s reverb anyway', false)),
      this.plugin ? el('p', { class: 'muted' }, 'The track\'s Layer Spatialize and Layer Room Send parameters do the same live and can be automated in Logic (Spatialize must be on here for the parameter to turn it on).') : '',
    ));
    // Leaders on offer: the listener and any layer that does not follow this one.
    const others = s.layers.map((_, j) => j).filter((j) => j !== i && !leadsTo(s.layers, j, i));
    const cur = !isLinked(l) ? 'none' : String(l.link!.to);
    const labels: Record<string, string> = { none: 'nothing', listener: 'the listener' };
    for (const j of others) labels[String(j)] = s.layers[j].name || `Layer ${j + 1}`;
    const to = select(['none', 'listener', ...others.map(String)], cur, (v) => u((x) => {
      if (v === 'none') delete x.link;
      else x.link = { ...(x.link ?? defaultLink('listener')), to: v === 'listener' ? 'listener' : parseInt(v, 10) };
    }), labels, 'none');
    const link = l.link;
    const nk = link?.keys.length ?? 0;
    const clear = el('button', { class: 'btn small' }, 'Clear');
    clear.addEventListener('click', () => u((x) => { if (x.link) x.link.keys = []; }));
    this.body.append(section('Link',
      row('Linked to', to),
      isLinked(l) ? row('From the start', checkbox(link!.linked_at_start, (v) => u((x) => { x.link!.linked_at_start = v; }), link!.linked_at_start ? 'linked' : 'free until a key links it', true)) : '',
      isLinked(l) ? el('p', { class: 'muted' }, 'While linked it keeps its place and bearing relative to that one as it moves and turns with its direction of travel (its own path, if any, goes with it). '
        + (nk ? `${nk} link key${nk === 1 ? '' : 's'} on the Link lane of the timeline. ` : 'Double-click the Link lane on the timeline to add keys that unlink and link it again at times. ')
        + 'Unlinking leaves it where it is.', nk ? clear : '')
        : el('p', { class: 'muted' }, 'Link the layer to the listener or another layer and it moves with it, keeping its distance and bearing; the timeline\'s Link lane unlinks and links it again at times.'),
    ));
  }

  // The layer's own path: drawing it, how it is timed, what happens at the
  // end, turning with it; and its level automation (fades).
  private renderLayerPath(i: number): void {
    const l = this.store.scene.layers[i];
    const u = (fn: (l: LayerDoc) => void, key?: string) => this.upd((s) => fn(s.layers[i]), key ? `${key}-${i}` : undefined);
    const drawing = this.tools.opts.layerTarget === i && this.tools.opts.tool !== 'select';
    const draw = el('button', { class: 'btn' + (hasPath(l) ? '' : ' primary') }, hasPath(l) ? 'Redraw' : 'Draw a path');
    draw.title = 'Draws with the tool chosen in the toolbar (the curve tool when none is): click points, Enter or double-click to finish';
    draw.addEventListener('click', () => this.tools.drawLayerPath(i));
    const cancel = el('button', { class: 'btn' }, 'Cancel');
    cancel.addEventListener('click', () => this.tools.setTool('select'));
    const rows: (Node | string)[] = [];
    if (drawing) {
      rows.push(el('p', { class: 'hint' }, `Drawing ${l.name || `Layer ${i + 1}`}'s path at its height: draw in the 3D view with the ${toolName(this.tools.opts.tool)}. `
        + 'The layer moves to where the path begins. Esc cancels.'), el('div', { class: 'btn-row' }, cancel));
    } else if (!hasPath(l)) {
      rows.push(el('p', { class: 'muted' }, 'Give the layer its own path and it travels along it: at its own speed, at the times you set, or held at a point you can automate. '
        + 'Click the button below, or pick any drawing tool in the toolbar while this layer is selected (the toolbar\'s "for" shows whose path you are drawing).'),
        el('div', { class: 'btn-row' }, draw));
    } else {
      const m = l.motion!;
      const length = layerPathLength(this.store.scene, this.store.analysis, i);
      const remove = el('button', { class: 'btn danger' }, 'Remove path');
      remove.addEventListener('click', () => u((x) => removePath(x)));
      const fit = el('button', { class: 'btn' }, 'Fit timing…');
      fit.title = 'Make this layer, others and the listener start and finish together';
      fit.addEventListener('click', () => this.setTab('path'));
      const arrival = layerArrival(l, length);
      const timing = select(['speed', 'keys', 'position'], m.timing, (v) => u((x) => {
        const mm = x.motion!;
        mm.timing = v as LayerTiming;
        // Keys to start from: setting off now and arriving when the speed
        // curve would have.
        if (v === 'keys' && mm.keys.length < 2) {
          const t0 = mm.start_time;
          const a = speedArrival(mm.speed, length);
          mm.keys = [{ time: t0, fraction: 0, easing: 'smooth' }, { time: Math.round((t0 + (a ?? 10)) * 100) / 100, fraction: 1, easing: 'smooth' }];
        }
      }), { speed: 'its speed (Layer speed lane)', keys: 'times to be at (Path % lane)', position: 'a point along the path' }, 'speed');
      const timingRows: (Node | string)[] = [];
      if (m.timing === 'speed') {
        timingRows.push(row('Sets off at', numberInput(m.start_time, (v) => u((x) => { x.motion!.start_time = Math.max(0, v); }), { step: 0.1, width: 64, def: 0 }), 's'),
          el('p', { class: 'muted' }, arrival !== null ? `Reaches the end at ${fmtTime(arrival, true)}. The Layer speed lane on the timeline shapes the journey; drag its red end line to make it quicker or slower.`
            : 'It stops on the way (its speed reaches 0) and never reaches the end.'));
      } else if (m.timing === 'keys') {
        timingRows.push(el('p', { class: 'muted' }, 'Each key on the Path % lane says where along the path the layer is at that time (0 % the start, 100 % the end). Double-click the lane to add one.'));
      } else {
        timingRows.push(row('Position', slider(m.fraction * 100, 0, 100, 0.1, (v) => u((x) => { x.motion!.fraction = v / 100; }, 'lfraction'),
          (v) => `${v.toFixed(1)} %`, () => this.store.endGesture(), 0)));
      }
      rows.push(
        row('Path', el('span', { class: 'muted' }, `${length.toFixed(1)} m${m.path.closed ? ', closed' : ''}`), draw, remove),
        row('Moves by', timing),
        ...timingRows,
        m.timing !== 'position' ? row('At the end', select(['stop', 'loop', 'ping_pong'], m.end, (v) => u((x) => { x.motion!.end = v as PathEnd; }),
          { stop: 'stops', loop: 'starts again', ping_pong: 'goes back and forth' }, 'stop')) : '',
        row('Turn', checkbox(m.turn, (v) => u((x) => { x.motion!.turn = v; }), 'turns with its direction of travel', false)),
        el('p', { class: 'muted' }, m.turn ? 'Its facing, stereo bar or sphere turn as the path turns (and round at each end going back and forth).'
          : 'It keeps facing the same way wherever it goes.'),
        this.plugin ? el('p', { class: 'muted' }, 'The track\'s Path Speed (×) and Path Position (%) parameters adjust this live and can be automated in Logic.') : '',
        el('p', { class: 'muted' }, 'Drag the layer to move it with its path; drag the path\'s points to reshape it (Option+click the line adds a point).'),
        el('div', { class: 'btn-row' }, fit),
      );
    }
    this.body.append(section('Path', ...rows));
    this.renderSpatializeAndLink(i);
    const keys = l.level_keys ?? [];
    const clear = el('button', { class: 'btn small' }, 'Clear');
    clear.addEventListener('click', () => u((x) => { delete x.level_keys; }));
    this.body.append(section('Fades',
      el('p', { class: 'muted' }, keys.length
        ? `${keys.length} level key${keys.length === 1 ? '' : 's'} on the Layer level lane (on top of the level above). Drag a key to the bottom of the lane for silence.`
        : 'Double-click the Layer level lane on the timeline to add level keys: fade the layer in and out (the bottom of the lane is silence).'
          + (this.plugin ? ' In Logic, the track\'s own volume automation does the same.' : '')),
      keys.length ? el('div', { class: 'btn-row' }, clear) : ''));
  }

  async addAudioFiles(): Promise<void> {
    const files = await this.backend.chooseAudioFiles();
    if (!files.length) return;
    for (const f of files) this.store.audioInfo.set(f.path, f);
    this.addLayers(files);
  }

  // One Ambisonic layer from separate mono files (one per channel), taken in
  // name order: W X Y Z sorts right for FuMa, 0 1 2 3 … for ambiX.
  async addAmbisonicFiles(): Promise<void> {
    const files = await this.backend.chooseAudioFiles('Choose the mono files of one Ambisonic recording (4, 9 or 16)');
    if (!files.length) return;
    for (const f of files) this.store.audioInfo.set(f.path, f);
    if (!AMBISONIC_CHANNELS.includes(files.length)) {
      this.flash(`An Ambisonic recording has 4, 9 or 16 channels; ${files.length} file${files.length === 1 ? ' was' : 's were'} chosen.`, 'error');
      return;
    }
    const bad = files.find((f) => f.error || f.channels !== 1);
    if (bad) {
      this.flash(bad.error ? `${bad.name}: ${bad.error}` : `${bad.name} has ${bad.channels} channels; each file must be mono.`, 'error');
      return;
    }
    const sorted = [...files].sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
    let name = sorted[0].name;
    for (const f of sorted) { let k = 0; while (k < name.length && k < f.name.length && name[k] === f.name[k]) k++; name = name.slice(0, k); }
    name = name.replace(/[\s_\-.]+$/, '') || sorted[0].name;
    this.addLayers([{ path: '', name, channels: files.length, files: sorted.map((f) => f.path) }]);
  }

  // New layers go on a ring around the listener's start, facing it; dropped
  // files go where they were dropped (in a row, 1.5 m apart, when there are
  // several). Stereo files become left/right pairs; 4 / 9 / 16-channel files
  // Ambisonic spheres.
  addLayers(files: { path: string; name: string; channels?: number; files?: string[] }[], at?: V3): void {
    const L = this.store.scene.listener;
    const first = L.paths[L.active_path]?.segments[0]?.points[0] ?? L.static_position;
    const r2 = (v: number) => Math.round(v * 100) / 100;
    this.upd((s) => {
      const n0 = s.layers.length;
      files.forEach((f, k) => {
        const i = n0 + k;
        let pos: V3;
        if (at) {
          pos = [r2(at[0] + (k - (files.length - 1) / 2) * 1.5), r2(at[1]), r2(at[2])];
        } else {
          const a = (i * 2.399963) % (Math.PI * 2);  // golden angle: spreads any number of layers evenly
          const r = 3 + 0.4 * Math.floor(i / 6);
          pos = [r2(first[0] - r * Math.sin(a)), 1.6, r2(first[2] - r * Math.cos(a))];
        }
        const channels = channelsForFile(f.channels || 1);
        s.layers.push(defaultLayer(i, { name: f.name || `Layer ${i + 1}`, audio: f.path, position: pos, home: [...pos] as V3, channels,
          ...(f.files ? { audio_files: f.files } : {}), ...layerExtras(channels) }));
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
      row('Height', numberInput(ed.draw_height ?? 1.7, (v) => this.upd((sc) => { sc.editor = { ...sc.editor, draw_height: v }; }), { step: 0.1, width: 64, def: 1.7 }), 'm (ear height)'),
      row('Snap', select(['0', '0.1', '0.25', '0.5', '1'], String(o.grid), (v) => { o.grid = parseFloat(v); this.render(true); },
        { '0': 'off', '0.1': '10 cm', '0.25': '25 cm', '0.5': '50 cm', '1': '1 m' }, '0')),
      row('', checkbox(o.append, (v) => { o.append = v; this.render(true); }, 'New strokes continue the active path', false)),
      row('Spiral', numberInput(o.spiralTurns, (v) => { o.spiralTurns = Math.max(0.25, v); }, { step: 0.5, width: 56, def: 3 }), 'turns'),
      row('Helix', numberInput(o.helixTurns, (v) => { o.helixTurns = Math.max(0.25, v); }, { step: 0.5, width: 56, def: 3 }), 'turns,',
        numberInput(o.helixRise, (v) => { o.helixRise = v; }, { step: 0.5, width: 56, def: 2 }), 'm rise'),
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
    const startPos = row('Start', numberInput(L.path_start_time, (v) => this.upd((sc) => { sc.listener.path_start_time = Math.max(0, v); }), { step: 0.5, width: 64, def: DL.path_start_time }), 's',
      checkbox(L.loop_path, (v) => this.upd((sc) => { sc.listener.loop_path = v; }), 'loop the path', DL.loop_path));
    const fraction = row('Position', slider(L.path_fraction * 100, 0, 100, 0.1, (v) => this.upd((sc) => { sc.listener.path_fraction = v / 100; }, 'fraction'),
      (v) => `${v.toFixed(1)} %`, () => this.store.endGesture(), DL.path_fraction * 100));
    this.body.append(section('Listener movement',
      row('Moves by', select(['speed', 'along_path'], L.position_mode, (v) => this.upd((sc) => { sc.listener.position_mode = v as 'speed' | 'along_path'; }),
        { speed: 'speed curve (timeline)', along_path: 'position along path' }, DL.position_mode)),
      L.position_mode === 'speed' ? startPos : fraction,
      L.paths.length ? '' : row('Stands at', vec3Inputs(L.static_position, (v) => this.upd((sc) => { sc.listener.static_position = v; }), 0.1, DL.static_position)),
    ));

    this.renderFit();

    // Head.
    const H = L.head;
    const layerNames = s.layers.map((l, i) => l.name || `Layer ${i + 1}`);
    const lookAt = H.mode === 'look_at' ? [
      row('Target', select(['-1', ...layerNames.map((_, i) => String(i))], String(H.look_at_layer),
        (v) => this.upd((sc) => { sc.listener.head.look_at_layer = parseInt(v, 10); }),
        Object.fromEntries([['-1', 'a point'], ...layerNames.map((n, i) => [String(i), n])]), '-1')),
      H.look_at_layer < 0 ? row('Point', vec3Inputs(H.look_at_point, (v) => this.upd((sc) => { sc.listener.head.look_at_point = v; }), 0.1, DL.head.look_at_point)) : '',
    ] : [];
    this.body.append(section('Head',
      row('Faces', select(['along_path', 'look_at', 'keyframed'], H.mode, (v) => this.upd((sc) => { sc.listener.head.mode = v as HeadMode; }),
        { along_path: 'where it walks', look_at: 'a target', keyframed: 'keyframes only' }, DL.head.mode)),
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

  // Fit timing: the listener and the chosen layers set off together and reach
  // the ends of their paths together; their speed curves stretch or squeeze.
  private renderFit(): void {
    const s = this.store.scene;
    const a = this.store.analysis;
    const L = s.listener;
    const listenerOk = L.paths.length > 0 && L.position_mode === 'speed';
    const layers = fittableLayers(s);
    if (!layers.length) return;
    const items: { id: string; name: string; start: number | null; end: number | null }[] = [];
    if (listenerOk) {
      const arr = speedArrival(L.speed, listenerPathLength(s, a));
      items.push({ id: 'L', name: 'Listener', start: L.path_start_time, end: arr === null ? null : L.path_start_time + arr });
    }
    for (const i of layers) {
      const l = s.layers[i];
      items.push({ id: String(i), name: l.name || `Layer ${i + 1}`, start: layerStart(l), end: layerArrival(l, layerPathLength(s, a, i)) });
    }
    const chosen = items.filter((x) => !this.fitOut.has(x.id));
    const starts = chosen.map((x) => x.start).filter((v): v is number => v !== null);
    const ends = chosen.map((x) => x.end).filter((v): v is number => v !== null);
    const r2 = (v: number) => Math.round(v * 100) / 100;
    const start = this.fitStart ?? r2(starts.length ? Math.min(...starts) : 0);
    const end = this.fitEnd ?? r2(ends.length ? Math.max(...ends) : start + 30);
    const list = el('div', { class: 'fit-list' });
    for (const x of items) {
      const when = x.start !== null ? `${fmtTime(x.start, true)} → ${x.end !== null ? fmtTime(x.end, true) : 'never arrives'}` : '';
      list.append(row('', checkbox(!this.fitOut.has(x.id), (v) => { if (v) this.fitOut.delete(x.id); else this.fitOut.add(x.id); this.render(true); }, x.name),
        el('span', { class: 'muted' }, when)));
    }
    const go = el('button', { class: 'btn primary' }, 'Fit') as HTMLButtonElement;
    go.disabled = !chosen.length || !(end > start);
    go.addEventListener('click', () => {
      this.upd((sc) => fitTiming(sc, this.store.analysis, { listener: chosen.some((x) => x.id === 'L'), layers: chosen.filter((x) => x.id !== 'L').map((x) => parseInt(x.id, 10)) }, start, end));
      this.fitStart = this.fitEnd = null;
    });
    this.body.append(section('Fit timing',
      el('p', { class: 'muted' }, 'Make the ticked ones set off together and reach the ends of their paths together. Each keeps the shape of its journey, quicker or slower; layers timed by keys have their keys spread over the same span.'),
      list,
      row('From', numberInput(start, (v) => { this.fitStart = Math.max(0, v); this.render(true); }, { step: 0.5, width: 64 }), 's  to',
        numberInput(end, (v) => { this.fitEnd = Math.max(0, v); this.render(true); }, { step: 0.5, width: 64 }), 's'),
      el('div', { class: 'btn-row' }, go),
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
        Object.fromEntries(MATERIALS.map((m) => [m, m.replace('_', ' ')])), DR.materials[w].name)));
    this.body.append(section('Space',
      row('Type', select(types, R.type, (v) => this.upd((sc) => { sc.room.type = v as SceneDoc['room']['type']; }),
        { box: 'room (box)', mesh: 'room (mesh)', outdoor: 'outdoors (ground only)', none: 'free field (no reflections)' }, DR.type)),
      mesh ? el('p', { class: 'muted' }, meshNote(R.mesh, this.info?.steamAudio)) : '',
      box ? row('Size', vec3Inputs(R.size, (v) => this.upd((sc) => { sc.room.size = v.map((x) => Math.max(1, x)) as typeof v; }), 0.5, DR.size)) : '',
      box ? el('p', { class: 'muted' }, 'Width (x), height (y), depth (z) in metres.') : '',
      box ? row('Centre', vec3Inputs(R.origin, (v) => this.upd((sc) => { sc.room.origin = v; }), 0.5, DR.origin)) : '',
    ));
    if (R.type !== 'none' && !mesh) this.body.append(section(box ? 'Surfaces' : 'Ground', ...mats));
    if (R.objects?.length) {
      this.body.append(section('Objects',
        ...R.objects.map((o) => row(o.name || 'object', el('span', { class: 'muted' }, `${o.material.name}, ${o.max.map((v, i) => (v - o.min[i]).toFixed(1)).join(' × ')} m`))),
        el('p', { class: 'muted' }, 'Walls and objects block and reflect sound when the ray-traced room model is in use. They are set in the scene file for now.')));
    }
    const db = (v: number) => `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`;
    if (R.type !== 'none') {
      this.body.append(section('Early reflections',
        row('Reflections', checkbox(R.reflections, (v) => this.upd((sc) => { sc.room.reflections = v; }), undefined, DR.reflections),
          select(['0', '1', '2', '3'], String(R.reflection_order), (v) => this.upd((sc) => { sc.room.reflection_order = parseInt(v, 10); }),
            { '0': 'none', '1': '1st order', '2': '2nd order', '3': '3rd order' }, String(DR.reflection_order))),
        row('Level', slider(R.reflections_level_db, -24, 12, 0.5, (v) => this.upd((sc) => { sc.room.reflections_level_db = v; }, 'refl-level'), db, () => this.store.endGesture(), DR.reflections_level_db)),
        el('p', { class: 'muted' }, 'Mirror images of each layer in the walls, moving with it. Walls scatter part of each echo into the reverb (the material\'s scattering).'),
      ));
    }
    if (box) this.renderLateReverb();
    this.body.append(el('p', { class: 'muted' }, 'Room changes restart the room model, so you may hear a short fade.'));
    this.body.append(section('Air',
      row('Temperature', numberInput(E.temperature_c, (v) => this.upd((sc) => { sc.environment.temperature_c = v; }), { step: 1, width: 64, def: DE.temperature_c }), '°C'),
      row('Humidity', numberInput(E.humidity, (v) => this.upd((sc) => { sc.environment.humidity = Math.max(0, Math.min(100, v)); }), { step: 5, width: 64, def: DE.humidity }), '%'),
      row('', checkbox(E.air_absorption, (v) => this.upd((sc) => { sc.environment.air_absorption = v; }), 'High-frequency loss over distance', DE.air_absorption)),
      row('Speed of sound', numberInput(E.speed_of_sound, (v) => this.upd((sc) => { sc.environment.speed_of_sound = Math.max(50, v); }), { step: 1, width: 64, def: DE.speed_of_sound }), 'm/s'),
    ));
  }

  // Late reverb: the built-in room model or a loaded impulse response.
  private renderLateReverb(): void {
    const R = this.store.scene.room;
    const ir = R.impulse_response;
    const usingIr = !!ir && ir.enabled;
    const db = (v: number) => `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`;
    const kind = select(['builtin', 'ir'], usingIr ? 'ir' : 'builtin', (v) => this.upd((sc) => {
      if (v === 'ir') sc.room.impulse_response = { file: '', gain_db: 0, channels: 0, ...(sc.room.impulse_response ?? {}), enabled: true };
      else if (sc.room.impulse_response) {
        if (sc.room.impulse_response.file) sc.room.impulse_response.enabled = false;
        else delete sc.room.impulse_response;
      }
    }), { builtin: 'Built-in (from the room)', ir: 'Impulse response (WAV)' }, 'builtin');
    const rows: (Node | string)[] = [
      row('Reverb', checkbox(R.reverb, (v) => this.upd((sc) => { sc.room.reverb = v; }), undefined, DR.reverb), kind),
      row('Level', slider(R.reverb_level_db, -24, 12, 0.5, (v) => this.upd((sc) => { sc.room.reverb_level_db = v; }, 'rev-level'), db, () => this.store.endGesture(), DR.reverb_level_db)),
    ];
    if (usingIr && ir) {
      const load = el('button', { class: 'btn small' }, ir.file ? 'Replace…' : 'Load IR…');
      load.addEventListener('click', async () => {
        const f = await this.backend.chooseFile('Load an impulse response', '*.wav;*.wave');
        if (!f) return;
        const infos = await this.backend.audioInfo([f.path]);
        for (const i of infos) this.store.audioInfo.set(i.path, i);
        this.upd((sc) => { sc.room.impulse_response = { gain_db: 0, channels: 0, ...(sc.room.impulse_response ?? {}), file: f.path, enabled: true }; });
      });
      const info = ir.file ? this.store.audioInfo.get(ir.file) : undefined;
      const name = ir.file ? ir.file.split(/[\\/]/).pop() ?? ir.file : 'none';
      const interp = (ch: number) => ch === 1 ? 'mono, played as a diffuse field' : ch === 2 ? 'stereo, left and right of the head'
        : ch >= 4 ? 'first-order ambiX, fixed to the room' : `${ch} channels: the first is used as mono`;
      const effective = ir.channels || info?.channels || 0;
      rows.push(
        row('IR file', el('span', { class: 'file', title: ir.file }, name), load),
        info && !info.error ? el('p', { class: 'muted' }, `${info.channels} ch · ${fmtTime(info.duration, true)} · ${(info.sampleRate / 1000).toFixed(1)} kHz — ${interp(effective)}.`) : '',
        info?.error ? el('p', { class: 'warn-text' }, `Cannot read the file: ${info.error}`) : '',
        row('Channels', select(['0', '1', '2', '4'], String(ir.channels), (v) => this.upd((sc) => { if (sc.room.impulse_response) sc.room.impulse_response.channels = parseInt(v, 10); }),
          { '0': 'as in the file', '1': 'mono (diffuse)', '2': 'stereo L / R', '4': 'ambiX (4 ch, world-fixed)' }, '0')),
        row('IR gain', slider(ir.gain_db, -24, 12, 0.5, (v) => this.upd((sc) => { if (sc.room.impulse_response) sc.room.impulse_response.gain_db = v; }, 'ir-gain'), db, () => this.store.endGesture())),
        el('p', { class: 'muted' }, 'The IR is scaled to the room\'s calibrated reverb level (gain is a trim on top) and arrives 4.7 ms late. '
          + 'It replaces the built-in tail only; turn the early reflections off above if the recording has its own.'),
      );
    } else {
      rows.push(row('Decay ×', numberInput(R.reverb_time_scale, (v) => this.upd((sc) => { sc.room.reverb_time_scale = Math.max(0.1, v); }), { step: 0.1, width: 64, def: DR.reverb_time_scale }),
        'of the room\'s Eyring RT60'));
    }
    this.body.append(section('Late reverb', ...rows));
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
        { binaural: 'Headphones (binaural)', speakers: 'Speakers', ambix: 'Ambisonics (ambiX, 3rd order)' }, 'binaural')),
      out.mode === 'speakers' ? row('Layout', select(LAYOUTS, out.layout, (v) => setOut('speakers', v), {}, '7.1.4')) : '',
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
      row('From', numberInput(this.bounceFrom, (v) => { this.bounceFrom = Math.max(0, v); }, { step: 1, width: 64, def: 0 }), 's  to',
        numberInput(end, (v) => { this.bounceTo = v === this.store.duration ? null : Math.max(0, v); }, { step: 1, width: 64, def: this.store.duration }), 's'),
      row('Sample rate', select(['44100', '48000', '88200', '96000'], String(rate), (v) => { this.bounceRate = parseInt(v, 10); }, {},
        String(info?.sampleRate ?? 48000))),
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

function toolName(t: string): string {
  return ({ freehand: 'freehand tool', polyline: 'point-to-point tool', curve: 'curve tool (click points, Enter to finish)', pen: 'pen tool', shape: 'shape tool' } as Record<string, string>)[t] ?? 'tool';
}

function layoutChannels(name: string): number {
  return ({ stereo: 2, quad: 4, '5.1': 6, '7.1': 8, '5.1.4': 10, '7.1.4': 12, '9.1.6': 16 } as Record<string, number>)[name] ?? 2;
}

function meshNote(mesh: unknown, steam: boolean | undefined): string {
  const file = typeof mesh === 'object' && mesh && 'file' in mesh ? String((mesh as { file: unknown }).file).split(/[\\/]/).pop() : null;
  const from = file ? `Shape from ${file}.` : 'Shape stored in the scene file.';
  return steam === false ? `${from} This build has no ray tracer, so a mesh room plays as free field.` : `${from} Surfaces come from the mesh's materials.`;
}
