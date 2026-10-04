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
const waitAnalysis = () => page.waitForFunction(() => {
  const s = window.spEditor.store;
  return s.analysis && s.analysis.revision === s.revision;
}, null, { timeout: 5000 });
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

await check('room tab edits the room', async () => {
  await page.click('.tab[data-tab=room]');
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
