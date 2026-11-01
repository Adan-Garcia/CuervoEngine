// Headless Firefox run of a web export, for the determinism matrix (milestone 11.1a).
//
//   node tools/web/firefox_smoke.mjs --dir <export dir> [--args "<runtime args>"]
//                                    [--expect "<log text>"] [--timeout 90] [--port 8766]
//                                    [--browser <firefox>]
//
// Like smoke_test.mjs but over WebDriver BiDi, since Firefox has no Chrome DevTools
// protocol: serves <dir> on 127.0.0.1, starts headless Firefox, opens
// index.html?args=<runtime args>, prints the page's console and passes when the engine logs
// --expect's text (default "at tick") and then "[web] engine exited" without errors.
// Needs Node 22+ (global WebSocket) and Firefox. Exit codes: 0 ok, 1 failed, 77 no browser.
// Firefox on Linux has no WebGPU by default, so this runs the WebGL2 fallback.

import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtempSync, readFileSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { extname, join, resolve } from 'node:path';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
    const i = args.indexOf(name);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const dir = resolve(opt('--dir', 'build/web/runtime'));
const port = Number(opt('--port', '8766'));
const timeout = Number(opt('--timeout', '90'));
const runtimeArgs = opt('--args', '--quit-after 5');
const expect = opt('--expect', 'at tick');

function findBrowser() {
    const explicit = opt('--browser', '');
    if (explicit) return explicit;
    for (const name of ['firefox', 'firefox-esr']) {
        try {
            return execFileSync('which', [name]).toString().trim();
        } catch { /* next */ }
    }
    return '';
}
const browserPath = findBrowser();
if (!browserPath) {
    console.error('firefox_smoke: no Firefox found');
    process.exit(77);
}

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
                '.data': 'application/octet-stream' };
const server = createServer((req, res) => {
    const path = join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
    if (!path.startsWith(dir)) { res.writeHead(403).end(); return; }
    try {
        const body = readFileSync(statSync(path).isDirectory() ? join(path, 'index.html') : path);
        res.writeHead(200, {
            'Content-Type': types[extname(path)] || 'application/octet-stream',
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        }).end(body);
    } catch {
        res.writeHead(404).end();
    }
});
await new Promise((ok) => server.listen(port, '127.0.0.1', ok));

const profile = mkdtempSync(join(tmpdir(), 'cuervo-ff-smoke-'));
const debugPort = port + 1000;
const browser = spawn(browserPath, ['--headless', '--no-remote', '--profile', profile,
    `--remote-debugging-port=${debugPort}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });

let passed = false;
let failed = false;
const finish = (code) => {
    browser.kill('SIGKILL');
    server.close();
    try { rmSync(profile, { recursive: true, force: true }); } catch { /* best effort */ }
    console.log(code === 0 ? 'firefox_smoke: OK' : 'firefox_smoke: FAILED');
    process.exit(code);
};
setTimeout(() => {
    console.error(`firefox_smoke: timed out after ${timeout} s`);
    finish(1);
}, timeout * 1000);

// Firefox prints "WebDriver BiDi listening on ws://127.0.0.1:<port>" once it is ready.
await new Promise((ok) => {
    let buffer = '';
    browser.stderr.on('data', (chunk) => {
        buffer += chunk.toString();
        if (buffer.includes('WebDriver BiDi listening')) ok();
    });
});
const ws = new WebSocket(`ws://127.0.0.1:${debugPort}/session`);
await new Promise((ok) => { ws.onopen = ok; });
let nextId = 1;
const pending = new Map();
const send = (method, params = {}) => new Promise((ok) => {
    const id = nextId++;
    pending.set(id, ok);
    ws.send(JSON.stringify({ id, method, params }));
});
ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    if (msg.id && pending.has(msg.id)) {
        pending.get(msg.id)(msg);
        pending.delete(msg.id);
        return;
    }
    if (msg.method === 'log.entryAdded') {
        const p = msg.params;
        const text = p.text ?? '';
        console.log(text);
        if (p.level === 'error' && !/^\[(info|warn)/.test(text) && /^\[error\]|Aborted|RuntimeError/.test(text)) failed = true;
        if (text.includes(expect)) passed = true;
        if (text.includes('[web] engine exited')) finish(passed && !failed ? 0 : 1);
    }
};
await send('session.new', { capabilities: {} });
await send('session.subscribe', { events: ['log.entryAdded'] });
const tree = await send('browsingContext.getTree', {});
const context = tree.result.contexts[0].context;
const url = `http://127.0.0.1:${port}/index.html?args=${encodeURIComponent(runtimeArgs)}`;
await send('browsingContext.navigate', { context, url, wait: 'none' });
