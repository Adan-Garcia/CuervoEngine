// Headless smoke test of the web export (docs/WEB.md "Verifying").
//
//   node tools/web/smoke_test.mjs [--dir build/web/runtime] [--seconds 5] [--port 8765]
//                                 [--browser <chrome>] [--screenshot <file.png>]
//                                 [--args "<runtime args>"] [--expect "<log text>"] [--gpu]
//                                 [--no-webgpu] [--expect-backend webgpu|gl]
//                                 [--pull <memfs path>=<file>] [--browser-flags "<flags>"]
//
// Serves <dir> on 127.0.0.1, starts headless Chrome/Chromium (SwiftShader
// WebGL2) with remote debugging, opens index.html?args=--quit-after <seconds>
// and collects the page's console over the DevTools protocol in real time.
// Passes when the engine logs "--quit-after reached" (or --expect's text)
// without errors/aborts. --args replaces the runtime arguments, e.g.
// `--args "--screenshot /tmp/s.ppm" --expect "state hash <hex>"` checks that
// the WASM simulation matches a native run tick for tick (DESIGN.md 4.1).
// --no-webgpu hides navigator.gpu from the page, as in a browser without
// WebGPU, so the WebGL2 fallback runs (milestone 10.3); --expect-backend fails
// the run unless the engine opened that device ("WebGPU device:" or
// "OpenGL device:" in the log). --pull copies a file the engine wrote into
// the page's MEMFS (e.g. its --screenshot PPM) out once the engine exits, so
// `rhi_parity compare` can check browser frames against native ones.
// Needs Node 22+ (global WebSocket; emsdk ships one) and Chrome or Chromium.
// Exit codes: 0 ok, 1 failed, 77 no browser.

import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { extname, join, resolve } from 'node:path';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
    const i = args.indexOf(name);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const dir = resolve(opt('--dir', 'build/web/runtime'));
const seconds = Number(opt('--seconds', '5'));
const port = Number(opt('--port', '8765'));
const screenshot = opt('--screenshot', '');
const runtimeArgs = opt('--args', `--quit-after ${seconds}`);
const expect = opt('--expect', '--quit-after reached');
const gpu = process.argv.includes('--gpu');
const browserFlags = opt('--browser-flags', '').split(' ').filter(Boolean);
const noWebgpu = process.argv.includes('--no-webgpu');
const expectBackend = opt('--expect-backend', '');
const pull = opt('--pull', '');
const backendLine = { webgpu: 'WebGPU device:', gl: 'OpenGL device:' }[expectBackend];
if (expectBackend && !backendLine) {
    console.error(`smoke_test: --expect-backend takes webgpu or gl, not '${expectBackend}'`);
    process.exit(1);
}

function findBrowser() {
    const explicit = opt('--browser', '');
    if (explicit) return explicit;
    for (const name of ['google-chrome-stable', 'google-chrome', 'chromium', 'chromium-browser']) {
        try {
            return execFileSync('which', [name]).toString().trim();
        } catch { /* next */ }
    }
    return '';
}

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
                '.data': 'application/octet-stream' };
const server = createServer((req, res) => {
    const path = join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
    if (!path.startsWith(dir)) { res.writeHead(403).end(); return; }
    try {
        const body = readFileSync(statSync(path).isDirectory() ? join(path, 'index.html') : path);
        // Cross-origin isolation: WASM threads need SharedArrayBuffer (docs/WEB.md "Threads");
        // harmless for single-threaded builds.
        res.writeHead(200, {
            'Content-Type': types[extname(path)] || 'application/octet-stream',
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        }).end(body);
    } catch {
        res.writeHead(404).end();
    }
});

const browserPath = findBrowser();
if (!browserPath) {
    console.error('smoke_test: no Chrome/Chromium found');
    process.exit(77);
}
await new Promise((ok) => server.listen(port, '127.0.0.1', ok));
const profile = mkdtempSync(join(tmpdir(), 'cuervo-web-smoke-'));
const browser = spawn(browserPath, [
    '--headless=new', '--no-sandbox', '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${profile}`, '--remote-debugging-port=0',
    // --gpu: the machine's GPU (benchmarks): WebGL2 through ANGLE on GL,
    // WebGPU through Dawn on Vulkan. Headless Chrome has no Vulkan surface, and
    // unless told not to use one its WebGPU only offers SwiftShader (milestone
    // 10.4). Default: SwiftShader, which runs anywhere (CI).
    ...(gpu ? ['--use-gl=angle', '--use-angle=gl', '--enable-gpu', '--enable-features=Vulkan',
               '--enable-unsafe-webgpu', '--disable-vulkan-surface']
            : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader']),
    '--ignore-gpu-blocklist',
    '--autoplay-policy=no-user-gesture-required', '--window-size=1280,800', ...browserFlags, 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

const lines = [];
let failed = false;
let passed = false;
let backendSeen = !backendLine;
const finish = (code) => {
    browser.kill('SIGKILL');
    server.close();
    try { rmSync(profile, { recursive: true, force: true }); } catch { /* best effort */ }
    console.log(code === 0 ? 'smoke_test: OK' : 'smoke_test: FAILED');
    process.exit(code);
};
setTimeout(() => {
    console.error(`smoke_test: timed out after ${seconds + 60} s`);
    finish(1);
}, (seconds + 60) * 1000);

// DevTools endpoint: Chrome prints "DevTools listening on ws://..." on stderr.
const wsUrl = await new Promise((ok) => {
    let buffer = '';
    browser.stderr.on('data', (chunk) => {
        buffer += chunk.toString();
        const m = buffer.match(/DevTools listening on (ws:\/\/\S+)/);
        if (m) ok(m[1]);
    });
});
const version = await (await fetch(wsUrl.replace('ws://', 'http://').replace(/\/devtools\/.*/, '/json/version'))).json();
void version;
const targets = await (await fetch(wsUrl.replace('ws://', 'http://').replace(/\/devtools\/.*/, '/json/list'))).json();
const page = targets.find((t) => t.type === 'page');
const ws = new WebSocket(page.webSocketDebuggerUrl);
let nextId = 1;
const pending = new Map();
const send = (method, params = {}) => new Promise((ok) => {
    const id = nextId++;
    pending.set(id, ok);
    ws.send(JSON.stringify({ id, method, params }));
});
ws.onmessage = async (event) => {
    const msg = JSON.parse(event.data);
    if (msg.id && pending.has(msg.id)) {
        pending.get(msg.id)(msg.result);
        pending.delete(msg.id);
        return;
    }
    if (msg.method === 'Runtime.consoleAPICalled') {
        const text = msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
        lines.push(text);
        console.log(text);
        if (/^\[error\]|Aborted|RuntimeError/.test(text)) failed = true;
        if (text.includes(expect)) passed = true;
        if (backendLine && text.includes(backendLine)) backendSeen = true;
        if (screenshot && (text.includes('OpenGL device:') || text.includes('WebGPU device:'))) {
            // Mid-game (the window is gone after the quit): halfway through the run.
            setTimeout(async () => {
                const shot = await send('Page.captureScreenshot', { format: 'png' });
                writeFileSync(screenshot, Buffer.from(shot.data, 'base64'));
                console.log(`[smoke] screenshot written to ${screenshot}`);
            }, (seconds * 1000) / 2);
        }
        if (text.includes('[web] engine exited')) {
            if (!backendSeen) console.error(`smoke_test: the engine never logged "${backendLine}"`);
            const pulled = pull ? await pullFile() : true;
            finish(passed && backendSeen && pulled && !failed ? 0 : 1);
        }
    } else if (msg.method === 'Runtime.exceptionThrown') {
        const d = msg.params.exceptionDetails;
        console.log(`[page exception] ${d.exception?.description ?? d.text}`);
        failed = true;
    }
};
// --pull: base64 through Runtime.evaluate. WebGPU captures land a frame or
// so after the engine logs them (an async readback), so retry for a while.
async function pullFile() {
    const [from, to] = pull.split('=');
    const expression = `(() => { try { const d = Module.FS.readFile(${JSON.stringify(from)}); let s = '';
        for (let i = 0; i < d.length; i += 0x8000) s += String.fromCharCode.apply(null, d.subarray(i, i + 0x8000));
        return btoa(s); } catch (e) { return ''; } })()`;
    for (let attempt = 0; attempt < 40; ++attempt) {
        const r = await send('Runtime.evaluate', { expression, returnByValue: true });
        const data = r?.result?.value;
        if (data) {
            writeFileSync(to, Buffer.from(data, 'base64'));
            console.log(`[smoke] pulled ${from} to ${to}`);
            return true;
        }
        await new Promise((ok) => setTimeout(ok, 100));
    }
    console.error(`smoke_test: ${from} never appeared in the page's file system`);
    return false;
}

await new Promise((ok) => { ws.onopen = ok; });
await send('Runtime.enable');
await send('Page.enable');
if (noWebgpu) {
    // Before any page script: the page then behaves as in a browser without WebGPU.
    await send('Page.addScriptToEvaluateOnNewDocument', {
        source: "Object.defineProperty(Navigator.prototype, 'gpu', { get: () => undefined, configurable: true });",
    });
}
await send('Page.navigate', { url: `http://127.0.0.1:${port}/index.html?args=${encodeURIComponent(runtimeArgs)}` });
