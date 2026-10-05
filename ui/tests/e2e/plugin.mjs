// End-to-end test of the editor in plugin mode: the page gets a stand-in for
// the plugin's native side (plugin/Source/PluginEditor.cpp) that owns the
// transport, the tracks and the scene document, as Logic and the scene
// instance do. Analysis still goes to the dev server's sp-scene.
// Run: npm run e2e (after run.mjs), needs the CMake build.
import { chromium } from 'playwright';
import { createServer } from 'vite';
import { mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const outDir = process.env.E2E_OUT ?? resolve(root, 'test-results');
mkdirSync(outDir, { recursive: true });

const server = await createServer({ root, configFile: resolve(root, 'vite.config.ts'), server: { port: 5198, strictPort: true }, logLevel: 'error' });
await server.listen();
const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));

// The plugin's side of the bridge, inside the page.
await page.addInitScript(() => {
  const listeners = new Map();
  const on = (id, fn) => { if (!listeners.has(id)) listeners.set(id, []); listeners.get(id).push(fn); };
  const fire = (id, payload) => { for (const fn of listeners.get(id) ?? []) fn(payload); };
  const host = {
    time: 0, playing: false, yaw: 0, calls: [], confirms: [], confirmAnswer: false,
    tracks: [{ id: 't-vox', name: 'Vox' }, { id: 't-gtr', name: 'Guitar' }],
    doc: null,
  };
  const layer = (name, id, pos) => ({ name, audio: '', position: pos, level_db: 0, mute: false, doppler: 1, spread_deg: 0,
    directivity: 0, directivity_forward: [0, 0, 1], reference_distance: 1, min_distance: 0.25, rolloff: 1, reverb_send_db: 0,
    reflection_order: -1, start_time: 0, loop: true, host_id: id });
  // The scene instance's reconcile: every live track has a layer.
  const reconcile = (doc) => {
    let changed = false;
    for (const t of host.tracks) {
      const l = doc.layers.find((x) => x.host_id === t.id);
      if (!l) { doc.layers.push(layer(t.name, t.id, [3 * doc.layers.length - 3, 1.6, -3])); changed = true; }
      else if (l.name !== t.name) { l.name = t.name; changed = true; }
    }
    return changed;
  };
  host.addTrack = (t) => {
    host.tracks.push(t);
    if (reconcile(host.doc)) fire('sceneReplaced', JSON.stringify(host.doc));
  };
  window.__host = host;
  const natives = {
    async analyze(a) {
      const r = await fetch('/api/analyze', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(a) });
      return r.json();
    },
    setScene(a) {
      host.doc = JSON.parse(JSON.stringify(a.doc ?? a.scene));
      if (reconcile(host.doc)) setTimeout(() => fire('sceneReplaced', JSON.stringify(host.doc)), 0);
      return { ok: true };
    },
    info() {
      return { device: 'Logic Pro', sampleRate: 48000, outputChannels: 2, cpu: 0, output: { mode: 'binaural', layout: '7.1.4' },
        status: 'Holds the scene', host: 'plugin', tracks: host.tracks };
    },
    startupScene() {
      if (!host.doc) {
        host.doc = { name: 'Logic session', duration: 0, layers: [], room: { type: 'box', size: [12, 3.5, 16] },
          listener: { paths: [], speed: [{ time: 0, speed: 1.4, easing: 'linear' }], static_position: [0, 1.7, 0] },
          environment: {} };
        reconcile(host.doc);
      }
      return { path: null, scene: host.doc, raw: host.doc };
    },
    audioInfo() { return []; },
    chooseAudioFiles() { return []; },
    confirm(a) { host.confirms.push(a.message); return { ok: host.confirmAnswer }; },
  };
  // Like WKWebView in JUCE on macOS: window.confirm answers Cancel without
  // showing anything, so the editor must ask through the native side.
  window.confirm = () => false;
  window.__JUCE__ = {
    initialisationData: { spHost: ['plugin'] },
    backend: {
      addEventListener: on,
      emitEvent(id, { name, params, resultId }) {
        if (id !== '__juce__invoke') return;
        host.calls.push(name);
        const arg = params.length ? JSON.parse(params[0]) : undefined;
        Promise.resolve(natives[name] ? natives[name](arg) : null).then((r) =>
          fire('__juce__complete', { promiseId: resultId, result: r == null ? '' : JSON.stringify(r) }));
      },
    },
  };
  setInterval(() => {
    if (host.playing) host.time += 1 / 30;
    fire('tick', { time: host.time, playing: host.playing, pose: [0, 1.7, 0, host.yaw, 0, 0, 0, 0], meters: [-12, -20], host: true });
  }, 33);
});

let failed = 0;
async function check(name, fn) {
  try {
    await fn();
    console.log(`ok   ${name}`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${name}: ${e.message}`);
    await page.screenshot({ path: resolve(outDir, `plugin-fail-${name.replace(/\W+/g, '_')}.png`) });
  }
}
const assert = (c, msg) => { if (!c) throw new Error(msg); };
const ed = (fn, arg) => page.evaluate(fn, arg);

await page.goto('http://localhost:5198/');
await page.waitForFunction(() => !!window.spEditor && window.spEditor.store.scene.layers.length === 2);

await check('the tracks are the layers; no audio files, no own transport', async () => {
  assert(await ed(() => window.spEditor.backend.kind) === 'plugin', 'backend kind');
  assert(await page.locator('text=Add audio files…').count() === 0, 'add audio button shown');
  assert(await page.locator('#toolbar >> text=Import…').count() === 1, 'no Import');
  assert(await page.locator('#toolbar >> text=Save as…').count() === 0, 'Save as shown');
  assert(!(await page.locator('button[title^="Play / pause"]').isVisible()), 'play button visible');
  const names = await ed(() => window.spEditor.store.scene.layers.map((l) => l.name));
  assert(JSON.stringify(names) === '["Vox","Guitar"]', JSON.stringify(names));
});

await check('the playhead and head direction follow the host while it is stopped', async () => {
  await ed(() => { window.__host.time = 12.5; window.__host.yaw = 40; });
  await page.waitForFunction(() => Math.abs(window.spEditor.store.time - 12.5) < 1e-6);
  const t = await page.textContent('.hud');
  assert(t.includes('40° left'), t);
});

await check('a layer shows its track and the name follows it', async () => {
  await page.locator('.layer-item').first().click();
  const track = page.locator('.row:has-text("Track") select');
  assert(await track.inputValue() === 't-vox', await track.inputValue());
  assert(await page.locator('.row:has-text("Name") input').isDisabled(), 'name editable');
  assert(await page.locator('text=Remove layer').count() === 0, 'remove offered for a bound layer');
});

await check('edits reach the plugin with the track binding', async () => {
  await ed(() => window.spEditor.store.update((s) => { s.layers[0].position = [1, 1.6, -4]; }));
  await page.waitForFunction(() => window.__host.doc.layers[0].position[2] === -4);
  const id = await ed(() => window.__host.doc.layers[0].host_id);
  assert(id === 't-vox', id);
});

await check('a new track in Logic adds a layer without losing undo', async () => {
  await ed(() => window.__host.addTrack({ id: 't-drm', name: 'Drums' }));
  await page.waitForFunction(() => window.spEditor.store.scene.layers.length === 3);
  assert(await ed(() => window.spEditor.store.scene.layers[2].host_id) === 't-drm', 'binding');
  assert(await ed(() => window.spEditor.store.canUndo), 'undo lost');
  assert(await ed(() => window.spEditor.store.scene.layers[0].position[2]) === -4, 'earlier edit lost');
});

await check('scrubbing asks for Logic instead of moving the playhead', async () => {
  const box = await page.locator('.tl-canvas').boundingBox();
  await ed(() => { window.__toasts = []; window.addEventListener('sp-message', (e) => window.__toasts.push(e.detail.text)); });
  await page.mouse.click(box.x + box.width * 0.6, box.y + 8);
  await page.waitForFunction(() => window.__toasts.some((t) => t.includes('playhead')), null, { timeout: 5000 });
  assert(Math.abs(await ed(() => window.spEditor.store.time) - 12.5) < 1e-6, 'time moved');
});

await check('Space, Return, comma and Cmd+S are left to Logic; the editor marks its own keys handled', async () => {
  const sent = async (init) => ed((init) => {
    const e = new KeyboardEvent('keydown', { ...init, bubbles: true, cancelable: true });
    window.dispatchEvent(e);
    return e.defaultPrevented;
  }, init);
  await ed(() => { window.__toasts = []; });
  assert(!(await sent({ key: ' ', code: 'Space' })), 'Space taken from Logic');
  assert(!(await sent({ key: 'Enter', code: 'Enter' })), 'Return taken from Logic');
  assert(!(await sent({ key: ',', code: 'Comma' })), 'comma taken from Logic');
  assert(!(await sent({ key: 's', code: 'KeyS', metaKey: true })), 'Cmd+S taken from Logic');
  assert(await sent({ key: 'c', code: 'KeyC' }), 'tool key not marked handled');
  assert(await ed(() => window.spEditor.tools.opts.tool) === 'curve', 'tool key ignored');
  assert(Math.abs(await ed(() => window.spEditor.store.time) - 12.5) < 1e-6, 'Enter moved the playhead');
  assert(await ed(() => window.__toasts.length) === 0, 'a hint was shown');
  await ed(() => window.spEditor.tools.setTool('select'));
});

await check('a path drawn while Logic stands at 12.5 s starts there', async () => {
  await page.keyboard.press('l');
  for (const p of [[-4, 1.7, -4], [4, 1.7, -4], [4, 1.7, 4]]) await page.mouse.click(...(await ed((p) => window.spEditor.project(p), p)));
  await page.keyboard.press('Enter');
  await page.waitForFunction(() => window.spEditor.store.scene.listener.paths.length === 1);
  const start = await ed(() => window.spEditor.store.scene.listener.path_start_time);
  assert(start === 12.5, `start ${start}`);
  await page.waitForFunction(() => window.__host.doc.listener.path_start_time === 12.5);
});

await check('Clear asks through the plugin and only clears on OK', async () => {
  await page.locator('#toolbar >> text=Clear').click();
  await page.waitForFunction(() => window.__host.confirms.length === 1);
  await page.waitForTimeout(200);
  assert(await ed(() => window.spEditor.store.scene.layers[0].position[2]) === -4, 'cleared on Cancel');
  await ed(() => { window.__host.confirmAnswer = true; });
  await page.locator('#toolbar >> text=Clear').click();
  await page.waitForFunction(() => window.spEditor.store.scene.layers.length === 3 && window.spEditor.store.scene.layers[0].position[2] === -3,
    null, { timeout: 5000 });
});

await page.screenshot({ path: resolve(outDir, 'plugin-mode.png') });
if (errors.length) { failed++; console.log(`FAIL page errors: ${errors.join(' | ')}`); }
await browser.close();
await server.close();
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
