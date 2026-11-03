// Runs a web build of the shmup in Chrome (smoke_test.mjs) and in Firefox
// (firefox_smoke.mjs) and requires the same state hash in both (milestone 11.9;
// DESIGN.md 4.1: the simulation is bit-identical across browsers).
//
//   node tools/web/browser_hash_check.mjs --dir <web build dir> [--expect-hash <hex>]
//                                         [--chrome <path>] [--firefox <path>]
//
// A missing browser is skipped (a note, exit 0) unless it is Chrome and there is
// nothing to compare; exit 1 on a failed run or a hash mismatch, 77 when neither
// browser exists. Both runs write their screenshot to /tmp/cuervo-hash-<browser>.ppm
// (the engine's --screenshot run reaches tick 240 and logs the hash).

import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
    const i = args.indexOf(name);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const dir = opt('--dir', 'build/web/runtime');
const expected = opt('--expect-hash', '');
const here = dirname(fileURLToPath(import.meta.url));

function run(script, extra, shot) {
    const r = spawnSync(process.execPath, [
        join(here, script), '--dir', dir, '--args', `--screenshot ${shot}`, '--expect', 'state hash', ...extra,
    ], { encoding: 'utf8' });
    const out = (r.stdout ?? '') + (r.stderr ?? '');
    const m = out.match(/state hash ([0-9a-f]{16})/);
    return { status: r.status, hash: m ? m[1] : '', out };
}

const results = {};
for (const [name, script, extra, port] of [
    ['chrome', 'smoke_test.mjs', [...(opt('--chrome', '') ? ['--browser', opt('--chrome', '')] : []), '--port', '8771'], 8771],
    ['firefox', 'firefox_smoke.mjs', [...(opt('--firefox', '') ? ['--browser', opt('--firefox', '')] : []), '--port', '8772'], 8772],
]) {
    void port;
    const r = run(script, extra, `/tmp/cuervo-hash-${name}.ppm`);
    if (r.status === 77) { console.log(`browser_hash_check: ${name} not found, skipped`); continue; }
    if (r.status !== 0 || !r.hash) {
        console.error(r.out.split('\n').filter((l) => !l.startsWith('[info')).slice(-15).join('\n'));
        console.error(`browser_hash_check: ${name} run failed`);
        process.exit(1);
    }
    results[name] = r.hash;
    console.log(`browser_hash_check: ${name} state hash ${r.hash}`);
}
const hashes = Object.values(results);
if (hashes.length === 0) process.exit(77);
if (new Set(hashes).size !== 1 || (expected && hashes[0] !== expected)) {
    console.error('browser_hash_check: MISMATCH');
    process.exit(1);
}
console.log('browser_hash_check: OK');
