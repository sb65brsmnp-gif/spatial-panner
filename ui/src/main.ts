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
import { Timeline } from './panels/timeline';
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

const toolbar = new Toolbar(tools, plugin, {
  newScene: () => {
    if (plugin) { if (confirm('Start over with an empty scene? The tracks stay as layers.')) store.replace({ ...defaultScene(), name: store.scene.name }); return; }
    if (!store.dirty || confirm('Discard unsaved changes?')) { store.load(defaultScene(), null); frame(); }
  },
  open: () => openScene(),
  save: (saveAs) => saveScene(saveAs),
  undo: () => store.undo(),
  redo: () => store.redo(),
  setView: (v) => setView(v),
  frame: () => frame(),
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
  if (b.isEmpty()) b.setFromCenterAndSize(new THREE.Vector3(0, 1, 0), new THREE.Vector3(12, 3, 12));
  return b;
}

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
  const r = b.getSize(new THREE.Vector3()).length() / 2;
  const dir = vp.persp.position.clone().sub(vp.controls.target).normalize();
  vp.controls.target.copy(c);
  vp.persp.position.copy(c.clone().add(dir.multiplyScalar(r / Math.sin((vp.persp.fov * Math.PI) / 360) * 0.7)));
  vp.controls.update();
}

function refreshToolbar(): void {
  const name = store.filePath ? store.filePath.split(/[\\/]/).pop()! : store.scene.name || 'Untitled scene';
  // Plugin: the scene is saved with the Logic project, so nothing is ever "unsaved".
  const title = `${name}${store.dirty && !plugin ? ' •' : ''}`;
  document.title = `${title} · Spatial Panner`;
  toolbar.refresh({ view: vp.view, follow, canUndo: store.canUndo, canRedo: store.canRedo, title });
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
  emptyHint.textContent = !s.layers.length ? (plugin ? 'Insert Spatial Panner on the tracks you want in the scene; each track becomes a layer.' : 'Add audio files (Layers tab), then draw the listener\'s path with a tool above.')
    : !s.listener.paths.length ? 'Draw the listener\'s path: pick Freehand, Point to point, Curve, Pen or a Shape above and draw on the floor.' : '';
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
  const missing = [...new Set(store.scene.layers.map((l) => l.audio).filter((p) => p && !store.audioInfo.has(p)))];
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
  // A mesh room is drawn from the analysis, which arrives after the scene.
  if (kinds.has('analysis') && store.scene.room.type === 'mesh') view.rebuildRoom();
  if (kinds.has('meters')) view.updateMeters();
  if (kinds.has('file')) backend.setOutput(store.scene.editor?.output ?? { mode: 'binaural', layout: '7.1.4' });
  if (kinds.has('scene') || kinds.has('analysis') || kinds.has('time') || kinds.has('transport')) updateListener();
  if (kinds.has('scene') || kinds.has('file') || kinds.has('tool') || kinds.has('selection')) refreshToolbar();
  if (kinds.has('time') || kinds.has('transport')) timeline.draw();
});

// --------------------------------------------------------------- files

async function openScene(): Promise<void> {
  if (plugin ? !confirm('Replace this session\'s scene with a scene file?') : store.dirty && !confirm('Discard unsaved changes?')) return;
  try {
    const r = await backend.openScene();
    if (r) loadOpened(r);
  } catch (e) {
    toast(`Could not open: ${e}`, 'error');
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

// ------------------------------------------------------------ messages

function toast(text: string, level: 'info' | 'warning' | 'error'): void {
  const t = el('div', { class: `toast ${level}` }, text);
  toasts.append(t);
  setTimeout(() => t.remove(), level === 'error' ? 8000 : 4000);
}

// ------------------------------------------------------------ keyboard

window.addEventListener('keydown', (e) => {
  const t = e.target as HTMLElement;
  const typing = t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA');
  const mod = e.metaKey || e.ctrlKey;
  if (mod && e.key.toLowerCase() === 'z') { e.preventDefault(); if (e.shiftKey) store.redo(); else store.undo(); return; }
  if (mod && e.key.toLowerCase() === 'y') { e.preventDefault(); store.redo(); return; }
  if (mod && e.key.toLowerCase() === 's') { e.preventDefault(); saveScene(e.shiftKey); return; }
  if (mod && e.key.toLowerCase() === 'o') { e.preventDefault(); openScene(); return; }
  if (typing || mod) return;
  const toolKeys: Record<string, ToolName> = { v: 'select', f: 'freehand', l: 'polyline', c: 'curve', p: 'pen', s: 'shape' };
  const viewKeys: Record<string, ViewName> = { '1': 'persp', '2': 'top', '3': 'front', '4': 'side', '5': 'listener' };
  const k = e.key.toLowerCase();
  if (toolKeys[k]) tools.setTool(toolKeys[k]);
  else if (viewKeys[k]) setView(viewKeys[k]);
});

window.addEventListener('beforeunload', (e) => { if (store.dirty && backend.kind === 'dev') e.preventDefault(); });

// ---------------------------------------------------------------- start

store.load(defaultScene(), null);
backend.onOpenFile((r) => {
  if (!store.dirty || confirm('Discard unsaved changes?')) loadOpened(r);
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
(window as unknown as Record<string, unknown>).spEditor = { store, tools, vp, view, backend, setView, project };
