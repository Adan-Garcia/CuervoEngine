// Web smoke for the 11.2 rendering features (docs/WEB.md "Verifying"): sprites (PNG and
// WebP), a 9-slice panel, built-in post-fx (bloom, vignette) and shaped Arabic text, in
// headless Chrome on WebGL2 and, with --gpu on a machine that has it, on WebGPU.
//
//   node tools/web/render_smoke.mjs --dir <web build dir of tools/web/render_game>
//        [--native-hash <hex>] [--native-ppm <file>] [--rhi-parity <path>] [--tolerance 8]
//        [--gpu] [--port 8780] [--browser <chrome>]
//
// Build the page (see tools/web/render_game/project.toml), run the same project natively with
// `cuervo --game tools/web/render_game --font <web_arabic.efont> --screenshot n.ppm` and pass its
// "state hash" and PPM. Each backend must log the same state hash as native; the browser
// frame is compared to the native one through `rhi_parity compare` (default tolerance
// is loose: SwiftShader and the native driver rasterize edges differently).
// Exit codes: 0 ok, 1 failed, 77 no browser.
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { readFileSync, writeFileSync } from 'node:fs';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
    const i = args.indexOf(name);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const dir = opt('--dir', 'build/render-web/runtime');
const hash = opt('--native-hash', '');
const nativePpm = opt('--native-ppm', '');
const parity = opt('--rhi-parity', '');
const tolerance = opt('--tolerance', '8');
// The page's canvas follows the browser window, so its capture is larger than the native
// 320x180 one: box-filter it to the native size before comparing (binary P6 PPM only).
function readPpm(file) {
    const b = readFileSync(file);
    const m = /^P6\s+(\d+)\s+(\d+)\s+255\s/.exec(b.subarray(0, 32).toString('latin1'));
    if (!m) throw new Error(`${file}: not a binary PPM`);
    return { w: +m[1], h: +m[2], px: b.subarray(m[0].length) };
}
function resample(src, w, h) {
    const out = Buffer.alloc(w * h * 3);
    for (let y = 0; y < h; ++y) for (let x = 0; x < w; ++x) {
        const x0 = Math.floor(x * src.w / w), x1 = Math.max(x0 + 1, Math.floor((x + 1) * src.w / w));
        const y0 = Math.floor(y * src.h / h), y1 = Math.max(y0 + 1, Math.floor((y + 1) * src.h / h));
        for (let c = 0; c < 3; ++c) {
            let sum = 0, n = 0;
            for (let yy = y0; yy < y1; ++yy) for (let xx = x0; xx < x1; ++xx) { sum += src.px[(yy * src.w + xx) * 3 + c]; ++n; }
            out[(y * w + x) * 3 + c] = Math.round(sum / n);
        }
    }
    return Buffer.concat([Buffer.from(`P6\n${w} ${h}\n255\n`), out]);
}
const smoke = join(dirname(fileURLToPath(import.meta.url)), 'smoke_test.mjs');

let code = 0;
const runs = [['webgl2', ['--no-webgpu', '--expect-backend', 'gl']]];
if (args.includes('--gpu')) runs.push(['webgpu', ['--gpu', '--expect-backend', 'webgpu']]);
for (const [name, extra] of runs) {
    const shot = join(tmpdir(), `cuervo-render-smoke-${name}-${process.pid}.ppm`);
    const r = spawnSync(process.execPath, [
        smoke, '--dir', dir, '--port', opt('--port', '8780'), '--args', `--screenshot ${shot}`,
        '--expect', hash ? `state hash ${hash}` : 'state hash',
        '--pull', `${shot}=${shot}`,
        ...extra, ...(opt('--browser', '') ? ['--browser', opt('--browser', '')] : []),
    ], { encoding: 'utf8' });
    const out = (r.stdout ?? '') + (r.stderr ?? '');
    if (r.status === 77) { console.log(`render_smoke: no browser, skipped`); process.exit(77); }
    if (r.status !== 0) {
        console.error(out.split('\n').slice(-15).join('\n'));
        console.error(`render_smoke: ${name} FAILED`);
        code = 1;
        continue;
    }
    console.log(`render_smoke: ${name} state hash ${(out.match(/state hash ([0-9a-f]{16})/) ?? [])[1] ?? '?'}`);
    if (nativePpm && parity) {
        const ref = readPpm(nativePpm);
        const scaled = shot.replace(/\.ppm$/, '-scaled.ppm');
        writeFileSync(scaled, resample(readPpm(shot), ref.w, ref.h));
        const c = spawnSync(parity, ['compare', nativePpm, scaled, '--tolerance', tolerance, '--max-fraction', '0.05'],
            { encoding: 'utf8' });
        process.stdout.write(c.stdout ?? '');
        if (c.status !== 0) { console.error(`render_smoke: ${name} screenshot differs from native`); code = 1; }
    }
}
console.log(code === 0 ? 'render_smoke: OK' : 'render_smoke: FAILED');
process.exit(code);
