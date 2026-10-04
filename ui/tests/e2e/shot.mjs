// Quick screenshot helper: node tests/e2e/shot.mjs URL out.png
import { chromium } from 'playwright';
const [url, out] = process.argv.slice(2);
const b = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
const p = await b.newPage({ viewport: { width: 1440, height: 900 } });
const logs = [];
p.on('console', (m) => logs.push(`${m.type()}: ${m.text()}`));
p.on('pageerror', (e) => logs.push(`pageerror: ${e.message}`));
await p.goto(url);
await p.waitForTimeout(2500);
await p.screenshot({ path: out });
console.log(logs.join('\n'));
await b.close();
