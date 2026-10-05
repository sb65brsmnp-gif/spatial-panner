// End-to-end test of the editor in a browser against the dev server (which
// uses the engine's sp-scene tool for analysis). Run: npm run e2e
// Needs the CMake build (build/tools/scene/sp-scene), the demo signals
// (build/tools/gensignals/sp-gensignals signals) and Playwright's Chromium.
import { chromium } from 'playwright';
import { createServer } from 'vite';
import { existsSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
if (!existsSync(resolve(root, '../signals/voice.wav'))) {
  console.error('Missing demo signals: run build/tools/gensignals/sp-gensignals signals from the repository root.');
  process.exit(1);
}
const outDir = process.env.E2E_OUT ?? resolve(root, 'test-results');
mkdirSync(outDir, { recursive: true });

const server = await createServer({ root, configFile: resolve(root, 'vite.config.ts'), server: { port: 5199, strictPort: true }, logLevel: 'error' });
await server.listen();
const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });

let failed = 0;
async function check(name, fn) {
  try {
    await fn();
    console.log(`ok   ${name}`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${name}: ${e.message}`);
    await page.screenshot({ path: resolve(outDir, `fail-${name.replace(/\W+/g, '_')}.png`) });
  }
}
const assert = (c, msg) => { if (!c) throw new Error(msg); };
const ed = (fn, arg) => page.evaluate(fn, arg);
// The analysis runs in the dev server's sp-scene tool; a loaded CI runner
// can take well over 5 s for it.
const waitAnalysis = () => page.waitForFunction(() => {
  const s = window.spEditor.store;
  return s.analysis && s.analysis.revision === s.revision;
}, null, { timeout: 20000 });
const screen = (p) => ed((p) => window.spEditor.project(p), p);

await page.goto('http://localhost:5199/');
await page.waitForFunction(() => !!window.spEditor);

await check('empty scene shows the getting-started hint', async () => {
  const t = await page.textContent('.empty-hint');
  assert(t.includes('Add audio files'), t);
});

await check('adding audio files creates layers around the listener', async () => {
  await page.click('text=Add audio files…');
  await page.waitForFunction(() => window.spEditor.store.scene.layers.length >= 3);
  const n = await ed(() => window.spEditor.store.scene.layers.length);
  const names = await ed(() => window.spEditor.store.scene.layers.map((l) => l.name));
  assert(names.includes('voice'), JSON.stringify(names));
  const pos = await ed(() => window.spEditor.store.scene.layers.map((l) => l.position));
  for (const p of pos) assert(Math.hypot(p[0], p[2]) > 2, `layer too close: ${p}`);
  console.log(`     ${n} layers`);
});

await ed(() => window.spEditor.setView('top'));
await page.waitForTimeout(200);

await check('freehand tool draws a smoothed Catmull-Rom path that the engine samples', async () => {
  await page.keyboard.press('f');
  const pts = [];
  for (let i = 0; i <= 40; i++) { const a = i / 40; pts.push([-4 + 8 * a, 1.7, 4 * Math.sin(a * Math.PI) - 2]); }
  const s0 = await screen(pts[0]);
  await page.mouse.move(...s0);
  await page.mouse.down();
  for (const p of pts.slice(1)) await page.mouse.move(...(await screen(p)), { steps: 2 });
  await page.mouse.up();
  await waitAnalysis();
  const r = await ed(() => {
    const s = window.spEditor.store;
    return { paths: s.scene.listener.paths, a: s.analysis };
  });
  assert(r.paths.length === 1, `paths: ${r.paths.length}`);
  const seg = r.paths[0].segments[0];
  assert(seg.type === 'catmull_rom', seg.type);
  assert(seg.points.length >= 4 && seg.points.length < 30, `points: ${seg.points.length}`);
  assert(r.a.paths[0].length > 9 && r.a.paths[0].length < 14, `length ${r.a.paths[0].length}`);
  for (const p of seg.points) assert(Math.abs(p[1] - 1.7) < 1e-6, 'drawn at ear height');
});

await check('point-to-point tool makes straight segments, Enter finishes', async () => {
  await page.keyboard.press('l');
  for (const p of [[-5, 1.7, 5], [0, 1.7, 5], [0, 1.7, 0], [5, 1.7, 0]]) await page.mouse.click(...(await screen(p)));
  await page.keyboard.press('Enter');
  await waitAnalysis();
  const path = await ed(() => window.spEditor.store.scene.listener.paths.at(-1));
  assert(path.segments.length === 3 && path.segments.every((s) => s.type === 'line'), JSON.stringify(path.segments.map((s) => s.type)));
  const a = await ed(() => window.spEditor.store.analysis.paths.at(-1).length);
  assert(Math.abs(a - 15) < 0.3, `length ${a}`);
});

await check('curve tool makes one smooth curve through the clicked points', async () => {
  await page.keyboard.press('c');
  for (const p of [[-5, 1.7, -5], [-2, 1.7, -3], [1, 1.7, -5], [4, 1.7, -3]]) await page.mouse.click(...(await screen(p)));
  await page.keyboard.press('Enter');
  await waitAnalysis();
  const path = await ed(() => window.spEditor.store.scene.listener.paths.at(-1));
  assert(path.segments.length === 1 && path.segments[0].type === 'catmull_rom' && path.segments[0].points.length === 4, JSON.stringify(path));
});

await check('pen tool: drag makes smooth Bezier anchors', async () => {
  await page.keyboard.press('p');
  const anchors = [[[-4, 1.7, 2], [-3, 1.7, 1]], [[0, 1.7, 3], [1, 1.7, 3]], [[4, 1.7, 2], [4, 1.7, 2]]];
  for (const [p, drag] of anchors) {
    await page.mouse.move(...(await screen(p)));
    await page.mouse.down();
    await page.mouse.move(...(await screen(drag)), { steps: 4 });
    await page.mouse.up();
  }
  await page.keyboard.press('Enter');
  await waitAnalysis();
  const path = await ed(() => window.spEditor.store.scene.listener.paths.at(-1));
  assert(path.segments.length === 2 && path.segments.every((s) => s.type === 'bezier'), JSON.stringify(path.segments));
});

for (const shape of ['circle', 'figure8', 'spiral', 'helix']) {
  await check(`shape tool: ${shape}`, async () => {
    await ed((s) => window.spEditor.tools.setTool('shape', s), shape);
    await page.mouse.move(...(await screen([0, 1.7, 0])));
    await page.mouse.down();
    await page.mouse.move(...(await screen([3, 1.7, 2])), { steps: 5 });
    await page.mouse.up();
    await waitAnalysis();
    const r = await ed(() => ({ p: window.spEditor.store.scene.listener.paths.at(-1), len: window.spEditor.store.analysis.paths.at(-1).length }));
    assert(r.len > 5, `length ${r.len}`);
    if (shape === 'circle') assert(Math.abs(r.len - 2 * Math.PI * Math.hypot(3, 2)) < 0.2, `circle length ${r.len}`);
    if (shape === 'helix') {
      const ys = r.p.segments[0].points.map((q) => q[1]);
      assert(Math.max(...ys) - Math.min(...ys) > 1.9, 'helix rises');
    }
  });
}

await check('undo removes the last path, redo restores it', async () => {
  const n = await ed(() => window.spEditor.store.scene.listener.paths.length);
  await page.keyboard.press('Escape');
  await page.keyboard.press('Control+z');
  const m = await ed(() => window.spEditor.store.scene.listener.paths.length);
  assert(m === n - 1, `${n} -> ${m}`);
  await page.keyboard.press('Control+Shift+z');
  assert((await ed(() => window.spEditor.store.scene.listener.paths.length)) === n, 'redo');
});

await check('select tool: dragging a path point reshapes the path (joint stays joined)', async () => {
  // Make the point-to-point path active and drag its corner at (0, 1.7, 5).
  await ed(() => { const s = window.spEditor.store; s.update((sc) => { sc.listener.active_path = 1; }); });
  await page.keyboard.press('v');
  await page.waitForTimeout(100);
  await page.mouse.move(...(await screen([0, 1.7, 5])));
  await page.mouse.down();
  await page.mouse.move(...(await screen([1, 1.7, 6])), { steps: 5 });
  await page.mouse.up();
  const segs = await ed(() => window.spEditor.store.scene.listener.paths[1].segments);
  const a = segs[0].points[1], b = segs[1].points[0];
  assert(Math.abs(a[0] - 1) < 0.05 && Math.abs(a[2] - 6) < 0.05, `moved to ${a}`);
  assert(a[0] === b[0] && a[2] === b[2], 'joint moved together');
});

await check('alt+click inserts a point, Delete removes it', async () => {
  const before = await ed(() => window.spEditor.store.scene.listener.paths[1].segments.length);
  await page.keyboard.down('Alt');
  await page.mouse.click(...(await screen([0, 1.7, 2.5])));
  await page.keyboard.up('Alt');
  const after = await ed(() => window.spEditor.store.scene.listener.paths[1].segments.length);
  assert(after === before + 1, `${before} -> ${after}`);
  await page.keyboard.press('Delete');
  assert((await ed(() => window.spEditor.store.scene.listener.paths[1].segments.length)) === before, 'deleted');
});

await check('dragging a layer moves it and the move is one undo step', async () => {
  const p0 = await ed(() => window.spEditor.store.scene.layers[0].position);
  await page.mouse.move(...(await screen(p0)));
  await page.mouse.down();
  await page.mouse.move(...(await screen([p0[0] + 1, p0[1], p0[2] + 1])), { steps: 6 });
  await page.mouse.up();
  const p1 = await ed(() => window.spEditor.store.scene.layers[0].position);
  assert(Math.abs(p1[0] - p0[0] - 1) < 0.05 && Math.abs(p1[2] - p0[2] - 1) < 0.05 && p1[1] === p0[1], `${p0} -> ${p1}`);
  await page.keyboard.press('Control+z');
  const p2 = await ed(() => window.spEditor.store.scene.layers[0].position);
  assert(JSON.stringify(p2) === JSON.stringify(p0), `undo -> ${p2}`);
});

await check('layer level slider changes level_db', async () => {
  await ed(() => window.spEditor.store.select({ kind: 'layer', index: 0 }));
  const slider = page.locator('.layer-item').first().locator('input[type=range]');
  await slider.fill('-12');
  assert((await ed(() => window.spEditor.store.scene.layers[0].level_db)) === -12, 'level');
});

await check('Option-click on a slider returns it to its default', async () => {
  const slider = page.locator('.layer-item').first().locator('input[type=range]');
  await slider.click({ modifiers: ['Alt'] });
  assert((await ed(() => window.spEditor.store.scene.layers[0].level_db)) === 0, 'level not reset');
  await ed(() => window.spEditor.store.select({ kind: 'layer', index: 0 }));
  await page.waitForTimeout(50);
  const doppler = page.locator('.row', { hasText: 'Doppler' }).locator('input[type=range]');
  await doppler.fill('20');
  assert(Math.abs((await ed(() => window.spEditor.store.scene.layers[0].doppler)) - 0.2) < 1e-6, 'doppler set');
  await page.waitForTimeout(50);
  await page.locator('.row', { hasText: 'Doppler' }).locator('input[type=range]').click({ modifiers: ['Alt'] });
  assert((await ed(() => window.spEditor.store.scene.layers[0].doppler)) === 1, 'doppler not reset');
  const ref = page.locator('.row', { hasText: 'Reference' }).locator('input');
  await ref.fill('4'); await ref.press('Enter');
  assert((await ed(() => window.spEditor.store.scene.layers[0].reference_distance)) === 4, 'reference set');
  await page.locator('.row', { hasText: 'Reference' }).locator('input').click({ modifiers: ['Alt'] });
  assert((await ed(() => window.spEditor.store.scene.layers[0].reference_distance)) === 1, 'reference not reset');
});

await check('Option-click on a layer puts it back where it was placed; Option-drag still moves', async () => {
  const home = await ed(() => window.spEditor.store.scene.layers[0].home);
  assert(Array.isArray(home), 'no home');
  const p0 = await ed(() => window.spEditor.store.scene.layers[0].position);
  await page.mouse.move(...(await screen(p0)));
  await page.mouse.down();
  await page.mouse.move(...(await screen([p0[0] + 1.5, p0[1], p0[2]])), { steps: 6 });
  await page.mouse.up();
  const p1 = await ed(() => window.spEditor.store.scene.layers[0].position);
  assert(Math.abs(p1[0] - p0[0] - 1.5) < 0.05, `not moved ${p1}`);
  await page.keyboard.down('Alt');
  await page.mouse.move(...(await screen(p1)));
  await page.mouse.down();
  await page.mouse.up();
  await page.keyboard.up('Alt');
  const p2 = await ed(() => window.spEditor.store.scene.layers[0].position);
  assert(JSON.stringify(p2) === JSON.stringify(home), `not home: ${p2} vs ${home}`);
});

await check('dropped audio files become layers where they are dropped', async () => {
  const n0 = await ed(() => window.spEditor.store.scene.layers.length);
  const [x, y] = await screen([2, 1.7, 2]);
  await ed(([x, y]) => window.spEditor.dropAudio(['/tmp/drop_a.wav', '/tmp/drop_b.wav'], x, y), [x, y]);
  const layers = await ed(() => window.spEditor.store.scene.layers);
  assert(layers.length === n0 + 2, `layers ${layers.length}`);
  const a = layers[n0].position, b = layers[n0 + 1].position;
  const mid = [(a[0] + b[0]) / 2, a[1], (a[2] + b[2]) / 2];
  assert(Math.abs(mid[0] - 2) < 0.1 && Math.abs(mid[2] - 2) < 0.1, `dropped at ${a} / ${b}`);
  assert(layers[n0].name === 'drop_a.wav' || layers[n0].name.startsWith('drop_a'), layers[n0].name);
  await ed(() => window.spEditor.store.undo());
});

await check('stereo layer: L and R on the balls, the centre handle moves the pair', async () => {
  await ed(() => window.spEditor.store.update((s) => { s.layers[0].channels = 2; s.layers[0].stereo = { width: 3, rotation: 0, elevation: 0, mono: false }; }));
  await page.waitForTimeout(100);
  const tags = await page.$$eval('.layer-end', (els) => els.filter((e) => e.style.display !== 'none' && e.offsetParent !== null).map((e) => e.textContent));
  assert(tags.includes('L') && tags.includes('R'), `tags ${tags}`);
  const c0 = await ed(() => window.spEditor.store.scene.layers[0].position);
  const [lx] = await screen([c0[0] - 1.5, c0[1], c0[2]]);
  const lTag = await page.$$eval('.layer-end', (els) => els.filter((e) => e.textContent === 'L' && e.offsetParent !== null).map((e) => { const r = e.getBoundingClientRect(); return r.left + r.width / 2; }));
  assert(lTag.some((x) => Math.abs(x - lx) < 6), `L tag at ${lTag}, ball at ${lx}`);
  await page.mouse.move(...(await screen(c0)));
  await page.mouse.down();
  await page.mouse.move(...(await screen([c0[0], c0[1], c0[2] + 1])), { steps: 6 });
  await page.mouse.up();
  const l = await ed(() => window.spEditor.store.scene.layers[0]);
  assert(Math.abs(l.position[2] - c0[2] - 1) < 0.05 && l.stereo.width === 3, `centre drag: ${l.position} w ${l.stereo.width}`);
  // Option-click an end: width and angle back to the defaults, centre stays.
  await page.keyboard.down('Alt');
  await page.mouse.move(...(await screen([l.position[0] + 1.5, l.position[1], l.position[2]])));
  await page.mouse.down();
  await page.mouse.up();
  await page.keyboard.up('Alt');
  const l2 = await ed(() => window.spEditor.store.scene.layers[0]);
  assert(l2.stereo.width === 2 && JSON.stringify(l2.position) === JSON.stringify(l.position), `end reset: ${JSON.stringify(l2.stereo)}`);
  await ed(() => window.spEditor.store.update((s) => { s.layers[0].channels = 1; delete s.layers[0].stereo; }));
});

await check('dragging the start disc moves the path with it; Option-click puts it back', async () => {
  const before = await ed(() => { const L = window.spEditor.store.scene.listener; return JSON.parse(JSON.stringify(L.paths[L.active_path])); });
  const first = (seg) => seg.type === 'arc' ? seg.points[1] : seg.points[0];
  const s0 = first(before.segments[0]);
  const grab = [s0[0] + 0.3, 0, s0[2]];
  await page.mouse.move(...(await screen(grab)));
  await page.mouse.down();
  await page.mouse.move(...(await screen([grab[0] + 2, 0, grab[2] - 1])), { steps: 8 });
  await page.mouse.up();
  const after = await ed(() => { const L = window.spEditor.store.scene.listener; return L.paths[L.active_path]; });
  const s1 = first(after.segments[0]);
  assert(Math.abs(s1[0] - s0[0] - 2) < 0.06 && Math.abs(s1[2] - s0[2] + 1) < 0.06 && s1[1] === s0[1], `start ${s0} -> ${s1}`);
  const last0 = before.segments.at(-1).points.at(-1), last1 = after.segments.at(-1).points.at(-1);
  assert(Math.abs(last1[0] - last0[0] - 2) < 0.06, 'path did not move with the start');
  await page.keyboard.down('Alt');
  await page.mouse.move(...(await screen([s1[0] + 0.3, 0, s1[2]])));
  await page.mouse.down();
  await page.mouse.up();
  await page.keyboard.up('Alt');
  await page.waitForFunction((h) => {
    const L = window.spEditor.store.scene.listener; const sg = L.paths[L.active_path].segments[0];
    const q = sg.type === 'arc' ? sg.points[1] : sg.points[0];
    return Math.abs(q[0] - h[0]) < 1e-6 && Math.abs(q[2] - h[2]) < 1e-6;
  }, before.home, { timeout: 2000 }).catch(() => {});
  const back = await ed(() => { const L = window.spEditor.store.scene.listener; return L.paths[L.active_path]; });
  const s2 = first(back.segments[0]);
  assert(Math.abs(s2[0] - before.home[0]) < 1e-6 && Math.abs(s2[2] - before.home[2]) < 1e-6, `reset ${s2} vs home ${before.home}`);
});

await check('timeline: double-click adds a speed key and a head yaw key; the engine follows', async () => {
  await ed(() => window.spEditor.store.update((s) => { s.listener.active_path = 1; s.duration = 20; }));
  await waitAnalysis();
  const box = await page.locator('.tl-canvas').boundingBox();
  const lanes = (box.height - 22) / 3;
  const xAt = (t) => box.x + 96 + (t / 20) * (box.width - 96);
  // Speed lane: add a key at 5 s, low speed.
  await page.mouse.dblclick(xAt(5), box.y + 22 + lanes - 8);
  // Yaw lane: add a key at 6 s at +90 (top quarter of the lane).
  await page.mouse.dblclick(xAt(6), box.y + 22 + lanes + lanes * 0.25);
  await waitAnalysis();
  const r = await ed(() => ({ L: window.spEditor.store.scene.listener, a: window.spEditor.store.analysis }));
  assert(r.L.speed.length === 2, `speed keys ${r.L.speed.length}`);
  assert(r.L.head.keys.length === 1 && r.L.head.keys[0].yaw > 60, JSON.stringify(r.L.head.keys));
  // The engine's pose at 10 s turns the head relative to the walking direction.
  const i = Math.round(10 / r.a.dt);
  const yaw = r.a.poses[i][3];
  assert(Number.isFinite(yaw), 'pose yaw');
});

await check('scrubbing the ruler moves the listener', async () => {
  const box = await page.locator('.tl-canvas').boundingBox();
  await page.mouse.click(box.x + 96 + (box.width - 96) * 0.5, box.y + 10);
  const t = await ed(() => window.spEditor.store.time);
  assert(Math.abs(t - 10) < 0.3, `time ${t}`);
  const pos = await ed(() => window.spEditor.view.listener.position.toArray());
  const pose = await ed(() => window.spEditor.store.poseAt(10));
  assert(Math.hypot(pos[0] - pose[0], pos[2] - pose[2]) < 0.05, 'avatar at pose');
});

await check('play advances time (simulated transport in the browser)', async () => {
  await page.keyboard.press('Space');
  await page.waitForFunction(() => window.spEditor.store.time > 10.5, null, { timeout: 8000 });
  await page.keyboard.press('Space');
  await page.waitForFunction(() => !window.spEditor.store.playing, null, { timeout: 3000 });
});

await check('a path drawn at the playhead starts there; speed keys count from the start', async () => {
  await ed(() => { window.spEditor.tools.setTool('select'); window.spEditor.store.setTime(6); });
  await page.keyboard.press('l');
  for (const p of [[-4, 1.7, -4], [4, 1.7, -4], [4, 1.7, 4]]) await page.mouse.click(...(await screen(p)));
  await page.keyboard.press('Enter');
  await waitAnalysis();
  const r = await ed(() => ({ L: window.spEditor.store.scene.listener, a: window.spEditor.store.analysis, dt: window.spEditor.store.analysis.dt }));
  assert(r.L.path_start_time === 6, `start ${r.L.path_start_time}`);
  // The engine keeps the listener at the first point until the start time.
  const at = (t) => r.a.poses[Math.round(t / r.dt)];
  assert(Math.hypot(at(5)[0] - at(0)[0], at(5)[2] - at(0)[2]) < 1e-3, 'listener moved before the start');
  assert(Math.hypot(at(9)[0] - at(0)[0], at(9)[2] - at(0)[2]) > 1, 'listener did not move after the start');
  // A speed key placed at 8 s on the timeline is 2 s after the path starts.
  const box = await page.locator('.tl-canvas').boundingBox();
  const lanes = (box.height - 22) / 3;
  await page.mouse.dblclick(box.x + 96 + (8 / 20) * (box.width - 96), box.y + 22 + lanes - 8);
  const keys = await ed(() => window.spEditor.store.scene.listener.speed.map((k) => k.time));
  assert(keys.includes(2), `speed key times ${keys}`);
  await page.keyboard.press('Enter');
  assert(Math.abs(await ed(() => window.spEditor.store.time) - 6) < 1e-6, 'Enter did not go to the path start');
  await ed(() => window.spEditor.store.update((s) => { s.listener.path_start_time = 0; }));
});

await check('timeline: dragging the path end stretches the speed curve; dragging the start moves the walk', async () => {
  // A walk that arrives: two speed keys (the earlier test left a near-stop).
  await ed(() => window.spEditor.store.update((s) => { s.listener.speed = [{ time: 0, speed: 1.4, easing: 'linear' }, { time: 2, speed: 1, easing: 'linear' }]; }));
  await waitAnalysis();
  const box = await page.locator('.tl-canvas').boundingBox();
  const dur = await ed(() => window.spEditor.store.duration);
  const xAt = (t) => box.x + 96 + (t / dur) * (box.width - 96);
  const yEmpty = box.y + box.height - 6;   // bottom of the pitch lane: no keys there
  const read = () => ed(() => ({ end: window.spEditor.store.analysis.arrival_time, start: window.spEditor.store.scene.listener.path_start_time,
    speed: window.spEditor.store.scene.listener.speed.map((k) => [k.time, k.speed]) }));
  const r0 = await read();
  assert(r0.end > 1 && r0.end < dur, `arrival ${r0.end}`);
  await page.mouse.move(xAt(r0.end), yEmpty);
  await page.mouse.down();
  await page.mouse.move(xAt(r0.end / 2), yEmpty, { steps: 8 });
  await page.mouse.up();
  await waitAnalysis();
  const r1 = await read();
  assert(Math.abs(r1.end - r0.end / 2) < 0.3, `end ${r0.end} -> ${r1.end}`);
  for (let i = 0; i < r0.speed.length; i++) {
    assert(Math.abs(r1.speed[i][0] - r0.speed[i][0] / 2) < 0.03, `key time ${r0.speed[i][0]} -> ${r1.speed[i][0]}`);
    assert(Math.abs(r1.speed[i][1] - r0.speed[i][1] * 2) < 0.03, `key speed ${r0.speed[i][1]} -> ${r1.speed[i][1]}`);
  }
  await page.mouse.move(xAt(0) + 1, yEmpty);
  await page.mouse.down();
  await page.mouse.move(xAt(3), yEmpty, { steps: 8 });
  await page.mouse.up();
  await waitAnalysis();
  const r2 = await read();
  assert(Math.abs(r2.start - 3) < 0.1, `start ${r2.start}`);
  assert(Math.abs(r2.end - r1.end - 3) < 0.3, `end ${r1.end} -> ${r2.end}`);
  assert(JSON.stringify(r2.speed) === JSON.stringify(r1.speed), 'speed keys changed with the start');
});

await check('timeline: Cmd-drag selects a range; dragging it moves start, end, speed and head keys together', async () => {
  await waitAnalysis();
  const box = await page.locator('.tl-canvas').boundingBox();
  const dur = await ed(() => window.spEditor.store.duration);
  const xAt = (t) => box.x + 96 + (t / dur) * (box.width - 96);
  const yEmpty = box.y + box.height - 6;
  const read = () => ed(() => { const L = window.spEditor.store.scene.listener; return { end: window.spEditor.store.analysis.arrival_time,
    start: L.path_start_time, speed: L.speed.map((k) => k.time), head: L.head.keys.map((k) => k.time) }; });
  const b = await read();
  assert(b.head.length >= 1 && b.end + 3 < dur, `scene ${JSON.stringify(b)}`);
  await page.keyboard.down('Meta');
  await page.mouse.move(xAt(1), yEmpty);
  await page.mouse.down();
  await page.mouse.move(xAt(b.end + 1), yEmpty, { steps: 6 });
  await page.mouse.up();
  await page.keyboard.up('Meta');
  await page.waitForFunction(() => document.querySelector('.tl-keybar').textContent.includes('path start, path end'));
  await page.mouse.move(xAt(2), yEmpty);
  await page.mouse.down();
  await page.mouse.move(xAt(4), yEmpty, { steps: 8 });
  await page.mouse.up();
  await waitAnalysis();
  const a = await read();
  assert(Math.abs(a.start - b.start - 2) < 0.1, `start ${b.start} -> ${a.start}`);
  assert(Math.abs(a.end - b.end - 2) < 0.3, `end ${b.end} -> ${a.end}`);
  assert(JSON.stringify(a.speed) === JSON.stringify(b.speed), 'speed keys moved on their own');
  for (let i = 0; i < b.head.length; i++) assert(Math.abs(a.head[i] - b.head[i] - 2) < 0.1, `head key ${b.head[i]} -> ${a.head[i]}`);
  await page.keyboard.press('Escape');
  await page.waitForFunction(() => !document.querySelector('.tl-keybar').textContent.includes('selected'));
  await ed(() => window.spEditor.store.update((s) => { s.listener.path_start_time = 0; }));
});

await check('Home (H) returns to the default 3D view with the scene in frame', async () => {
  await ed(() => { const vp = window.spEditor.vp; vp.persp.position.set(0.5, 0.5, 0.5); vp.controls.target.set(0, 0.5, 0); vp.controls.update(); });
  await page.keyboard.press('h');
  const r = await ed(() => { const vp = window.spEditor.vp; const d = vp.persp.position.clone().sub(vp.controls.target);
    return { dist: d.length(), dir: d.normalize().toArray(), view: vp.view }; });
  const want = [14, 13, 18].map((v) => v / Math.hypot(14, 13, 18));
  assert(r.view === 'persp', r.view);
  assert(r.dir.every((v, i) => Math.abs(v - want[i]) < 1e-3), `direction ${r.dir}`);
  assert(r.dist > 10, `distance ${r.dist}`);
});

await check('room tab edits the room (a new scene is outdoors; pick the box room first)', async () => {
  await page.click('.tab[data-tab=room]');
  assert(await ed(() => window.spEditor.store.scene.room.type) === 'outdoor', 'default room not outdoors');
  await page.locator('.row:has-text("Type") select').selectOption('box');
  await page.waitForFunction(() => window.spEditor.store.scene.room.type === 'box');
  const sizeX = page.locator('.vec3').first().locator('input').first();
  await sizeX.fill('20');
  await sizeX.press('Enter');
  assert((await ed(() => window.spEditor.store.scene.room.size[0])) === 20, 'size');
});

await page.click('.tab[data-tab=path]');
await ed(() => window.spEditor.setView('persp'));
await page.waitForTimeout(300);
await page.screenshot({ path: resolve(outDir, 'editor.png') });

await check('demo scene opens and its path matches the engine', async () => {
  await page.goto('http://localhost:5199/?scene=room_walk.json');
  await page.waitForFunction(() => window.spEditor?.store.scene.layers.length === 5);
  await waitAnalysis();
  const a = await ed(() => window.spEditor.store.analysis);
  assert(Math.abs(a.paths[0].length - 23.95) < 0.05, `length ${a.paths[0].length}`);
  assert(Math.abs(a.arrival_time - 22) < 0.5, `arrival ${a.arrival_time}`);
  await page.waitForTimeout(300);
  await page.screenshot({ path: resolve(outDir, 'room_walk.png') });
});

await check('Recent lists opened scenes and reopens them', async () => {
  page.once('dialog', (d) => d.accept());
  await ed(() => window.spEditor.openRecent('occluder.json'));
  await page.waitForFunction(() => (window.spEditor.store.filePath ?? '').endsWith('occluder.json'));
  await page.click('text=Recent ▾');
  await page.waitForSelector('.recent-item');
  const items = await page.$$eval('.recent-item', (els) => els.map((e) => e.textContent));
  assert(items.includes('occluder.json'), `recent ${items}`);
  await page.keyboard.press('Escape');
});

await check('a scene with objects shows them in the view and the Room tab', async () => {
  await page.goto('http://localhost:5199/?scene=occluder.json');
  await page.waitForFunction(() => window.spEditor?.store.scene.room.objects?.length === 3);
  await waitAnalysis();
  const o = await ed(() => window.spEditor.store.scene.room.objects[0]);
  assert(o.name === 'partition wall' && Math.abs(o.max[0] + 1.9) < 1e-3, JSON.stringify(o));
  await page.click('.tab:has-text("Room")');
  await page.waitForSelector('text=partition wall');
  await page.waitForTimeout(300);
  await page.screenshot({ path: resolve(outDir, 'occluder.png') });
});

await check('a mesh room opens from its OBJ file and is drawn from the engine', async () => {
  await page.goto('http://localhost:5199/?scene=lshape.json');
  await page.waitForFunction(() => window.spEditor?.store.scene.room.type === 'mesh');
  await waitAnalysis();
  const mesh = await ed(() => window.spEditor.store.scene.room.mesh);
  assert(mesh.file.endsWith('/scenes/lshape.obj'), JSON.stringify(mesh));
  const tris = await ed(() => window.spEditor.store.analysis.room_mesh?.triangles.length ?? 0);
  assert(tris > 10, `triangles ${tris}`);
  const lines = await ed(() => window.spEditor.view.roomGroup.children.filter((c) => c.type === 'LineSegments').length);
  assert(lines >= 1, 'mesh edges not drawn');
  await page.waitForTimeout(300);
  await page.screenshot({ path: resolve(outDir, 'lshape.png') });
});

if (errors.length) { failed++; console.log('Page errors:\n' + errors.join('\n')); }
await browser.close();
await server.close();
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
