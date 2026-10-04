// Vite config for the editor.
//
// `npm run build` produces dist/index.html with all JS and CSS inlined, which
// the app embeds as one resource. `npm run dev` serves the editor in a
// browser with a small API backed by the engine's sp-scene tool, so the
// editor runs and can be tested without the app (no audio).
import { defineConfig, type Plugin } from 'vite';
import { spawn } from 'node:child_process';
import { existsSync, readdirSync, readFileSync, openSync, readSync, closeSync } from 'node:fs';
import { dirname, join, resolve, basename, isAbsolute } from 'node:path';
import type { IncomingMessage, ServerResponse } from 'node:http';

const repo = resolve(__dirname, '..');
const spScene = process.env.SP_SCENE_BIN ?? join(repo, 'build', 'tools', 'scene', 'sp-scene');

function runScene(args: string[], input: string): Promise<string> {
  return new Promise((ok, fail) => {
    if (!existsSync(spScene)) return fail(new Error(`sp-scene not found at ${spScene}; build the repo with CMake first`));
    const p = spawn(spScene, args);
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    p.on('error', fail);
    p.on('close', () => ok(out));
    p.stdin.end(input);
  });
}

function body(req: IncomingMessage): Promise<any> {
  return new Promise((ok) => {
    let s = '';
    req.on('data', (d) => (s += d));
    req.on('end', () => ok(s ? JSON.parse(s) : {}));
  });
}

function send(res: ServerResponse, v: unknown, raw = false): void {
  res.setHeader('content-type', 'application/json');
  res.end(raw ? (v as string) : JSON.stringify(v));
}

// Duration of a PCM/float WAV from its header.
function wavInfo(path: string) {
  const fd = openSync(path, 'r');
  const buf = Buffer.alloc(4096);
  readSync(fd, buf, 0, 4096, 0);
  closeSync(fd);
  let pos = 12, channels = 1, rate = 48000, bits = 16, dataBytes = 0;
  while (pos + 8 <= buf.length) {
    const id = buf.toString('ascii', pos, pos + 4), size = buf.readUInt32LE(pos + 4);
    if (id === 'fmt ') { channels = buf.readUInt16LE(pos + 10); rate = buf.readUInt32LE(pos + 12); bits = buf.readUInt16LE(pos + 22); }
    if (id === 'data') { dataBytes = size; break; }
    pos += 8 + size + (size & 1);
  }
  return { channels, sampleRate: rate, duration: dataBytes / (channels * (bits / 8) * rate) };
}

function devApi(): Plugin {
  return {
    name: 'sp-dev-api',
    configureServer(server) {
      server.middlewares.use(async (req, res, next) => {
        try {
          if (req.url === '/api/analyze' && req.method === 'POST') {
            const { scene, duration } = await body(req);
            return send(res, await runScene(['analyze', '--duration', String(duration ?? 0)], JSON.stringify(scene)), true);
          }
          if (req.url === '/api/signals') {
            const dir = join(repo, 'signals');
            const files = existsSync(dir) ? readdirSync(dir).filter((f) => f.endsWith('.wav')) : [];
            return send(res, files.map((f) => ({ path: join(dir, f), name: basename(f, '.wav'), ...wavInfo(join(dir, f)) })));
          }
          if (req.url === '/api/scenes') {
            return send(res, readdirSync(join(repo, 'scenes')).filter((f) => f.endsWith('.json')));
          }
          if (req.url === '/api/open' && req.method === 'POST') {
            const { name } = await body(req);
            const path = join(repo, 'scenes', basename(name));
            const text = readFileSync(path, 'utf8');
            const scene = JSON.parse(await runScene(['normalize', '--base', dirname(path)], text));
            if (scene.error) return send(res, scene);
            for (const l of scene.layers) if (l.audio && !isAbsolute(l.audio)) l.audio = resolve(dirname(path), l.audio);
            // As in the app: keep a mesh room's file reference (made absolute), not the inlined triangles.
            const raw = JSON.parse(text);
            let mesh = raw.room?.mesh;
            if (typeof mesh === 'string') mesh = { file: mesh };
            if (mesh?.file) scene.room.mesh = { ...mesh, file: isAbsolute(mesh.file) ? mesh.file : resolve(dirname(path), mesh.file) };
            return send(res, { path, scene, raw });
          }
        } catch (e) {
          res.statusCode = 500;
          return send(res, { error: String(e) });
        }
        next();
      });
    },
  };
}

// Non-ASCII characters as \uXXXX escapes (valid in JS strings, template
// literals and regexes), so the page does not depend on the WebView honouring
// the charset of a resource-provider response.
const asciiOnly = (js: string) => js.replace(/[\u0080-\uffff]/g, (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`);

// Inlines the built JS and CSS into index.html so the app embeds one file.
function singleFile(): Plugin {
  return {
    name: 'sp-single-file',
    enforce: 'post',
    generateBundle(_opts, bundle) {
      const html = bundle['index.html'];
      if (!html || html.type !== 'asset') return;
      let src = String(html.source);
      for (const [name, chunk] of Object.entries(bundle)) {
        if (chunk.type === 'chunk' && chunk.isEntry) {
          src = src.replace(new RegExp(`<script[^>]*src="[^"]*${name}"[^>]*></script>`),
            () => `<script type="module">${asciiOnly(chunk.code).replace(/<\/script/g, '<\\/script')}</script>`);
          delete bundle[name];
        } else if (chunk.type === 'asset' && name.endsWith('.css')) {
          src = src.replace(new RegExp(`<link[^>]*href="[^"]*${name}"[^>]*>`), () => `<style>${String(chunk.source)}</style>`);
          delete bundle[name];
        }
      }
      html.source = src;
    },
  };
}

export default defineConfig({
  base: './',
  plugins: [devApi(), singleFile()],
  build: {
    outDir: 'dist',
    assetsInlineLimit: 100_000_000,
    cssCodeSplit: false,
    chunkSizeWarningLimit: 2000,
    rollupOptions: { output: { inlineDynamicImports: true } },
  },
  server: { port: 5173, strictPort: true },
});
