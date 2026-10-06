// Entry point: builds the editor and keeps the native engine in sync with it.
import * as THREE from 'three';
import './styles.css';
import { store } from './model/store';
import { defaultScene, engineScene, mergeEditorKeys, type SceneDoc } from './model/scene';
import { createBackend } from './bridge/backend';
import { Viewport, type ViewName } from './view/viewport';
import { SceneView } from './view/sceneView';
import { Interaction, type ToolName } from './view/interaction';
import { Toolbar } from './panels/toolbar';
import { Sidebar } from './panels/sidebar';
import { Timeline, transportKeyAction } from './panels/timeline';
import { el, fmtTime } from './panels/dom';

const backend = createBackend();
const plugin = backend.kind === 'plugin';
if (plugin) document.body.classList.add('plugin');
const app = document.getElementById('app')!;
const viewportEl = el('div', { id: 'viewport' });
const hud = el('div', { class: 'hud' });
const emptyHint = el('div', { class: 'empty-hint' });
const toasts = el('div', { class: 'toasts' });
viewportEl.append(hud, emptyHint, toasts);

const vp = new Viewport(viewportEl);
const view = new SceneView(vp, store);
const tools = new Interaction(vp, view, store);
let follow = false;

async function newScene(): Promise<void> {
  if (plugin) {
    if (await backend.confirm('Start over with an empty scene? The tracks stay as layers.', 'Clear')) store.replace({ ...defaultScene(), name: store.scene.name });
    return;
  }
  if (!store.dirty || await backend.confirm('Discard unsaved changes?', 'Discard')) { store.load(defaultScene(), null); frame(); }
}

const toolbar = new Toolbar(tools, plugin, {
  newScene: () => newScene(),
  open: () => openScene(),
  recent: () => backend.recentScenes(),
  openRecent: (path) => openRecent(path),
  clearRecent: () => backend.clearRecentScenes(),
  save: (saveAs) => saveScene(saveAs),
  undo: () => store.undo(),
  redo: () => store.redo(),
  setView: (v) => setView(v),
  frame: () => frame(),
  home: () => homeView(),
  toggleFollow: () => { follow = !follow; refreshToolbar(); },
});
const sidebar = new Sidebar(store, backend, tools);
const timeline = new Timeline(store, backend);
tools.blockDelete = () => timeline.keyFocused;

const main = el('div', { id: 'main' }, viewportEl, sidebar.root);
app.append(toolbar.root, main, timeline.root);

// ------------------------------------------------------------------ views

function sceneBounds(): THREE.Box3 {
  const b = new THREE.Box3();
  const s = store.scene;
  if (s.room.type === 'box') {
    const [w, h, d] = s.room.size, o = s.room.origin;
    b.expandByPoint(new THREE.Vector3(o[0] - w / 2, o[1], o[2] - d / 2));
    b.expandByPoint(new THREE.Vector3(o[0] + w / 2, o[1] + h, o[2] + d / 2));
  }
  for (const l of s.layers) b.expandByPoint(new THREE.Vector3(...l.position));
  for (const p of store.analysis?.paths ?? []) for (const q of p.points) b.expandByPoint(new THREE.Vector3(...q));
  for (const t of store.analysis?.layers ?? []) for (const q of t.points) b.expandByPoint(new THREE.Vector3(...q));
  b.expandByPoint(new THREE.Vector3(...s.listener.static_position));
  // Without a box room (outdoor, open, mesh) the ground around the origin is
  // part of the picture, and a scene of one layer is not a point: framing a
  // point would put the camera on it, where orbiting and zooming do nothing.
  if (s.room.type !== 'box' || b.isEmpty()) b.union(new THREE.Box3(new THREE.Vector3(-6, 0, -6), new THREE.Vector3(6, 3, 6)));
  const size = b.getSize(new THREE.Vector3());
  if (Math.min(size.x, size.z) < 4) b.expandByVector(new THREE.Vector3(Math.max(0, 4 - size.x) / 2, 0, Math.max(0, 4 - size.z) / 2));
  return b;
}

// The default 3D camera direction: from the front right, above.
const HOME_DIR = new THREE.Vector3(14, 13, 18).normalize();

function setView(v: ViewName): void {
  const b = sceneBounds();
  const c = b.getCenter(new THREE.Vector3());
  const size = b.getSize(new THREE.Vector3());
  const extent = v === 'top' ? Math.max(size.x, size.z) : v === 'front' ? Math.max(size.x, size.y * 2) : Math.max(size.z, size.y * 2);
  vp.setView(v, c, extent + 2);
  tools.applyControls();
  view.updateListener(store.poseAt(store.time));
  refreshToolbar();
}

function frame(): void {
  if (vp.view !== 'persp') return setView(vp.view);
  const b = sceneBounds();
  const c = b.getCenter(new THREE.Vector3());
  const r = Math.max(2, b.getSize(new THREE.Vector3()).length() / 2);
  let dir = vp.persp.position.clone().sub(vp.controls.target);
  // A camera sitting on its target has no direction: take the default one.
  dir = dir.lengthSq() < 1e-6 || !Number.isFinite(dir.lengthSq()) ? HOME_DIR.clone() : dir.normalize();
  vp.controls.target.copy(c);
  vp.persp.position.copy(c.clone().add(dir.multiplyScalar(r / Math.sin((vp.persp.fov * Math.PI) / 360) * 0.7)));
  vp.controls.update();
}

// Home: the 3D view from its default angle with the scene in frame.
function homeView(): void {
  vp.controls.target.set(0, 1, 0);
  vp.persp.position.copy(HOME_DIR.clone().multiplyScalar(26).add(vp.controls.target));
  setView('persp');
  frame();
}

function refreshToolbar(): void {
  const name = store.filePath ? store.filePath.split(/[\\/]/).pop()! : store.scene.name || 'Untitled scene';
  // Plugin: the scene is saved with the Logic project, so nothing is ever "unsaved".
  const title = `${name}${store.dirty && !plugin ? ' •' : ''}`;
  document.title = `${title} · Spatial Panner`;
  toolbar.refresh({ view: vp.view, follow, canUndo: store.canUndo, canRedo: store.canRedo, title,
    layers: store.scene.layers.map((l, i) => l.name || `Layer ${i + 1}`) });
}
tools.onToolChange = refreshToolbar;

// --------------------------------------------------------------- HUD

function updateHud(): void {
  const p = store.poseAt(store.time);
  if (!p) { hud.textContent = ''; return; }
  const yaw = p[3], pitch = p[4];
  hud.innerHTML = `<b>${fmtTime(store.time, true)}</b> &nbsp; listener (${p[0].toFixed(1)}, ${p[1].toFixed(1)}, ${p[2].toFixed(1)}) m`
    + ` &nbsp; head ${Math.abs(yaw).toFixed(0)}° ${yaw >= 0 ? 'left' : 'right'}, ${Math.abs(pitch).toFixed(0)}° ${pitch >= 0 ? 'up' : 'down'}`
    + (p[7] > 0.01 ? ` &nbsp; ${p[7].toFixed(1)} m/s` : '');
  const s = store.scene;
  // While a drawing tool is out, say whose path it draws: the selected
  // layer's, or the listener's ("for" in the toolbar changes it).
  const target = tools.opts.tool !== 'select' ? s.layers[tools.opts.layerTarget] : undefined;
  emptyHint.textContent = tools.opts.tool !== 'select'
    ? (target ? `Drawing the path of ${target.name || `Layer ${tools.opts.layerTarget + 1}`} at its height. For the listener's path instead, choose Listener under "for" in the toolbar. Esc cancels.`
      : s.layers.length ? 'Drawing the listener\'s path. For a layer\'s own path, choose it under "for" in the toolbar, or press Esc and select the layer first.' : '')
    : !s.layers.length ? (plugin ? 'Insert Spatial Panner on the tracks you want in the scene; each track becomes a layer.' : 'Add audio files (Layers tab), then draw the listener\'s path with a tool above.')
      // Until anything has been drawn; a scene whose layers move while the
      // listener stands still (common in Logic) is not waiting for a path.
      : !s.listener.paths.length && !s.layers.some((l) => l.motion?.path.segments.length) ? 'Draw the listener\'s path: pick Freehand, Point to point, Curve, Pen or a Shape above and draw on the floor.' : '';
  emptyHint.style.display = emptyHint.textContent ? '' : 'none';
}

function updateListener(): void {
  const pose = store.poseAt(store.time);
  view.updateListener(pose);
  if (follow && pose && vp.view === 'persp') {
    const target = new THREE.Vector3(pose[0], pose[1], pose[2]);
    const delta = target.clone().sub(vp.controls.target);
    vp.controls.target.add(delta);
    vp.persp.position.add(delta);
    vp.controls.update();
  }
  updateHud();
}

// ----------------------------------------------------------- engine sync

let syncTimer: number | null = null;
let lastSync = 0;
let latestApplied = -1;

function scheduleSync(): void {
  if (syncTimer !== null) return;
  const wait = Math.max(0, 50 - (performance.now() - lastSync));
  syncTimer = window.setTimeout(() => { syncTimer = null; lastSync = performance.now(); sync(); }, wait);
}

async function sync(): Promise<void> {
  const rev = store.revision;
  const scene = engineScene(store.scene);
  const duration = store.duration;
  await requestAudioInfo();
  backend.setScene(scene, duration, plugin ? store.scene : undefined).then((r) => { if (r && !r.ok && r.error) toast(r.error, 'error'); }).catch((e) => toast(String(e), 'error'));
  try {
    const a = await backend.analyze(scene, duration);
    if (a.error) { toast(a.error, 'error'); return; }
    if (rev < latestApplied) return;
    latestApplied = rev;
    a.revision = rev;
    store.setAnalysis(a);
  } catch (e) {
    toast(`Analysis failed: ${e}`, 'error');
  }
}

async function requestAudioInfo(): Promise<void> {
  const ir = store.scene.room.impulse_response?.file;
  const missing = [...new Set([...store.scene.layers.map((l) => l.audio), ir ?? ''].filter((p) => p && !store.audioInfo.has(p)))];
  if (!missing.length) return;
  const infos = await backend.audioInfo(missing);
  for (const i of infos) {
    store.audioInfo.set(i.path, i);
    if (i.error) toast(`${i.name}: ${i.error}`, 'warning');
  }
}

backend.onTick((t) => {
  const wasPlaying = store.playing;
  const host = !!t.host;
  const moved = host && Math.abs(t.time - store.time) > 1e-6;
  store.hostDriven = host;
  store.hostActive = t.active !== false;
  store.playing = t.playing;
  if (t.playing || wasPlaying || host) store.time = t.time;
  store.livePose = t.pose ?? null;
  if (t.meters) { store.meters = t.meters; store.emit('meters'); }
  // The host's automation turns the head even while the playhead stands.
  if (t.playing || wasPlaying !== t.playing || moved || host) store.emit('time');
  if (wasPlaying !== t.playing) store.emit('transport');
});
backend.onMessage((m) => toast(m.text, m.level));
backend.onSceneReplaced((doc) => store.replace(mergeEditorKeys(doc, doc)));
window.addEventListener('sp-message', (e) => { const d = (e as CustomEvent).detail; toast(d.text, d.level); });

async function refreshInfo(): Promise<void> {
  try { sidebar.info = await backend.info(); sidebar.render(); } catch { /* app not ready */ }
}
setInterval(refreshInfo, 2000);

store.subscribe((kinds) => {
  if (kinds.has('scene')) {
    scheduleSync();
    view.rebuildRoom();
    view.updateLayers();
    view.rebuildPaths();
  } else {
    if (kinds.has('selection')) { view.updateLayers(); view.rebuildPaths(); }
    if (kinds.has('analysis')) view.rebuildPaths();
  }
  // Layers with paths move with the playhead.
  if (!kinds.has('scene') && !kinds.has('selection') && (kinds.has('time') || kinds.has('analysis') || kinds.has('transport'))
    && store.scene.layers.some((l) => l.motion)) view.updateLayers();
  // A mesh room is drawn from the analysis, which arrives after the scene.
  if (kinds.has('analysis') && store.scene.room.type === 'mesh') view.rebuildRoom();
  if (kinds.has('meters')) view.updateMeters();
  if (kinds.has('file')) backend.setOutput(store.scene.editor?.output ?? { mode: 'binaural', layout: '7.1.4' });
  if (kinds.has('scene') || kinds.has('analysis') || kinds.has('time') || kinds.has('transport')) updateListener();
  if (kinds.has('scene') || kinds.has('file') || kinds.has('tool') || kinds.has('selection')) refreshToolbar();
  if (kinds.has('tool')) updateHud();
  if (kinds.has('time') || kinds.has('transport')) timeline.draw();
});

// --------------------------------------------------------------- files

async function openScene(): Promise<void> {
  if (plugin ? !await backend.confirm('Replace this session\'s scene with a scene file?', 'Import…')
    : store.dirty && !await backend.confirm('Discard unsaved changes?', 'Discard')) return;
  try {
    const r = await backend.openScene();
    if (r) loadOpened(r);
  } catch (e) {
    toast(`Could not open: ${e}`, 'error');
  }
}

// Open Recent (toolbar list or the app's File menu).
async function openRecent(path: string): Promise<void> {
  if (plugin ? !await backend.confirm('Replace this session\'s scene with a scene file?', 'Import')
    : store.dirty && !await backend.confirm(`Discard unsaved changes and open ${path.split(/[\\/]/).pop()}?`, 'Discard')) return;
  try {
    const r = await backend.openScenePath(path);
    if (r) loadOpened(r);
  } catch (e) {
    toast(`Could not open: ${e instanceof Error ? e.message : e}`, 'error');
  }
}

function loadOpened(r: { path: string | null; scene: SceneDoc; raw: unknown }): void {
  store.audioInfo.clear();
  const doc = mergeEditorKeys(r.scene, r.raw);
  if (plugin && r.path) {
    // Importing into a Logic session: layers named like a track play that
    // track; the session keeps its own document, so there is no file to save.
    const tracks = sidebar.info?.tracks ?? [];
    const taken = new Set<string>();
    for (const l of doc.layers) {
      const t = l.host_id ? undefined : tracks.find((x) => x.name === l.name && !taken.has(x.id));
      if (t) l.host_id = t.id;
      if (l.host_id) taken.add(l.host_id);
    }
    store.replace(doc);
  } else {
    store.load(doc, plugin ? null : r.path);
  }
  setTimeout(frame, 100);
}

async function saveScene(saveAs: boolean): Promise<void> {
  try {
    const r = await backend.saveScene(store.scene, saveAs || plugin ? null : store.filePath);
    if (!r) return;
    if (plugin) { toast(`Exported ${r.path.split(/[\\/]/).pop()}`, 'info'); return; }
    store.filePath = r.path;
    store.dirty = false;
    store.emit('file');
    toast(`Saved ${r.path.split(/[\\/]/).pop()}`, 'info');
  } catch (e) {
    toast(`Could not save: ${e}`, 'error');
  }
}

// ------------------------------------------------------- drag and drop

// Where a drop at (x, y) in the page lands in the scene: on the drawing
// plane under the pointer, or null outside the 3D view.
function dropPoint(x: number, y: number): [number, number, number] | null {
  const r = vp.renderer.domElement.getBoundingClientRect();
  if (x < r.left || x > r.right || y < r.top || y > r.bottom) return null;
  const h = store.scene.editor?.draw_height ?? 1.7;
  const hit = vp.intersect({ clientX: x, clientY: y } as MouseEvent, vp.editPlane(new THREE.Vector3(0, h, 0)));
  if (!hit || hit.length() > 500) return null;
  return [hit.x, hit.y, hit.z];
}

// Mono and stereo audio files dropped on the window become layers where they
// were dropped (on the ring around the start when dropped on the panels).
async function dropAudio(paths: string[], x: number, y: number): Promise<void> {
  if (plugin) { toast('In Logic each track is a layer: insert Spatial Panner on a track to add it.', 'info'); return; }
  if (!paths.length) return;
  const infos = await backend.audioInfo(paths);
  const ok = [];
  for (const i of infos) {
    store.audioInfo.set(i.path, i);
    if (i.error) toast(`${i.name}: ${i.error}`, 'error');
    else if (i.channels > 2) toast(`${i.name} has ${i.channels} channels; drop takes mono and stereo files. Use Add audio files… in the Layers tab for it.`, 'warning');
    else ok.push(i);
  }
  if (!ok.length) return;
  sidebar.addLayers(ok, dropPoint(x, y) ?? undefined);
  sidebar.setTab('layers');
}

backend.onDropFiles((d) => { viewportEl.classList.remove('file-drop'); void dropAudio(d.paths, d.x, d.y); });
backend.onDropHover((over) => viewportEl.classList.toggle('file-drop', over));
// The browser preview (npm run dev) gets the drop itself, with names only.
if (backend.kind === 'dev') {
  window.addEventListener('dragover', (e) => { e.preventDefault(); viewportEl.classList.add('file-drop'); });
  window.addEventListener('dragleave', (e) => { if (!e.relatedTarget) viewportEl.classList.remove('file-drop'); });
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    viewportEl.classList.remove('file-drop');
    const names = [...(e.dataTransfer?.files ?? [])].map((f) => f.name);
    void dropAudio(names, e.clientX, e.clientY);
  });
}

// The app's File menu (macOS menu bar).
backend.onMenu((m) => {
  if (m.action === 'new') void newScene();
  else if (m.action === 'open') void openScene();
  else if (m.action === 'save') void saveScene(false);
  else if (m.action === 'saveAs') void saveScene(true);
  else if (m.action === 'openRecent' && m.path) void openRecent(m.path);
});

// ------------------------------------------------------------ messages

function toast(text: string, level: 'info' | 'warning' | 'error'): void {
  const t = el('div', { class: `toast ${level}` }, text);
  toasts.append(t);
  setTimeout(() => t.remove(), level === 'error' ? 8000 : 4000);
}

// ------------------------------------------------------------ keyboard

// Plugin: a key the editor leaves alone (no preventDefault) goes on to
// Logic, which runs its own key command for it (the web view hands unhandled
// keys back to the window). So the transport keys, Save and Open are Logic's
// there, and a key the editor does use is marked handled so Logic's command
// on the same key stays quiet.
window.addEventListener('keydown', (e) => {
  const t = e.target as HTMLElement;
  const typing = t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA');
  const mod = e.metaKey || e.ctrlKey;
  if (mod && e.key.toLowerCase() === 'z') { e.preventDefault(); if (e.shiftKey) store.redo(); else store.undo(); return; }
  if (mod && e.key.toLowerCase() === 'y') { e.preventDefault(); store.redo(); return; }
  if (mod && e.key.toLowerCase() === 's') { if (plugin) return; e.preventDefault(); saveScene(e.shiftKey); return; }
  if (mod && e.key.toLowerCase() === 'o') { if (plugin) return; e.preventDefault(); openScene(); return; }
  if (typing || mod) return;
  // Transport: Enter returns to the beginning of the path, , and . step the
  // playhead (Space, play/pause, is the timeline's). While a line is being
  // drawn, Enter finishes it instead (the drawing tools run first and mark
  // the event handled). In the plugin these are Logic's keys: the plug-in
  // hands them to Logic before the page sees them (WebViewKeys.mm); should
  // one arrive here anyway, it is left alone.
  const transport = transportKeyAction(e);
  if (plugin && (transport || e.code === 'Space')) return;
  if (transport) {
    if (e.defaultPrevented) return;
    if (transport.kind === 'start') { if (tools.drawing) return; timeline.goToPathStart(); }
    else timeline.nudge(transport.seconds);
    e.preventDefault();
    return;
  }
  const toolKeys: Record<string, ToolName> = { v: 'select', f: 'freehand', l: 'polyline', c: 'curve', p: 'pen', s: 'shape' };
  const viewKeys: Record<string, ViewName> = { '1': 'persp', '2': 'top', '3': 'front', '4': 'side', '5': 'listener' };
  const k = e.key.toLowerCase();
  if (toolKeys[k]) { tools.setTool(toolKeys[k]); e.preventDefault(); }
  else if (viewKeys[k]) { setView(viewKeys[k]); e.preventDefault(); }
  else if (k === 'h') { homeView(); e.preventDefault(); }
});

// Plugin: while a text field has focus, Logic's transport keys are typed
// into it; otherwise the plug-in hands them to Logic (WebViewKeys.mm).
if (plugin) {
  const editable = (t: EventTarget | null): boolean => {
    const h = t as HTMLElement | null;
    if (!h || !h.tagName) return false;
    if (h.tagName === 'TEXTAREA' || h.isContentEditable) return true;
    return h.tagName === 'INPUT' && !['checkbox', 'range', 'button', 'radio', 'file', 'color'].includes((h as HTMLInputElement).type);
  };
  document.addEventListener('focusin', (e) => { if (editable(e.target)) backend.editing(true).catch(() => { /* no native side */ }); });
  document.addEventListener('focusout', (e) => { if (editable(e.target)) backend.editing(false).catch(() => { /* no native side */ }); });
}

window.addEventListener('beforeunload', (e) => { if (store.dirty && backend.kind === 'dev') e.preventDefault(); });

// ---------------------------------------------------------------- start

store.load(defaultScene(), null);
backend.onOpenFile(async (r) => {
  if (!store.dirty || await backend.confirm(`Discard unsaved changes and open ${r.path.split(/[\\/]/).pop()}?`, 'Discard')) loadOpened(r);
});
// A scene given on the command line / opened from the Finder (app), or ?scene=name.json (dev).
backend.startupScene().then((r) => { if (r) loadOpened(r); }).catch((e) => toast(String(e), 'error'));
setView('persp');
frame();
refreshInfo();

// For tests and debugging.
function project(p: [number, number, number]): [number, number] {
  const r = vp.renderer.domElement.getBoundingClientRect();
  const v = new THREE.Vector3(...p).project(vp.camera);
  return [(v.x * 0.5 + 0.5) * r.width + r.left, (-v.y * 0.5 + 0.5) * r.height + r.top];
}
(window as unknown as Record<string, unknown>).spEditor = { store, tools, vp, view, backend, setView, project, dropAudio, openRecent };
