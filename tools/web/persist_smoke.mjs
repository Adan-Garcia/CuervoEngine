// Reload-persistence smoke test for the IndexedDB save backend (milestone 11.9).
//
//   node tools/web/persist_smoke.mjs --dir <web build dir of tools/web/persist_game>
//                                    [--browser <chrome>] [--port 8770]
//
// The web build runs tools/web/persist_game, which loads the "boot" save slot,
// adds one to its counter, commits it and prints "persist-runs <n>". This runs
// it three times in one browser profile (so the same IndexedDB): the counter
// must read 1, 2, 3, which fails if saves vanish on reload (docs/WEB.md
// "Persistent saves"). Build:
//   cmake --preset web -B build/persist-web -DCUERVO_WEB_GAME=$PWD/tools/web/persist_game
// Exit codes follow smoke_test.mjs (0 ok, 1 failed, 77 no browser).

import { spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
    const i = args.indexOf(name);
    return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const dir = opt('--dir', 'build/persist-web');
const browser = opt('--browser', '');
const port = Number(opt('--port', '8770'));
const smoke = join(dirname(fileURLToPath(import.meta.url)), 'smoke_test.mjs');
const profile = mkdtempSync(join(tmpdir(), 'cuervo-persist-'));

let code = 0;
for (let run = 1; run <= 3 && code === 0; ++run) {
    const r = spawnSync(process.execPath, [
        smoke, '--dir', dir, '--port', String(port), '--profile', profile,
        '--args', '--quit-after 2', '--expect', `persist-runs ${run}`,
        ...(browser ? ['--browser', browser] : []),
    ], { stdio: 'inherit' });
    if (r.status === 77) { code = 77; break; }
    if (r.status !== 0) {
        console.error(`persist_smoke: run ${run} did not log "persist-runs ${run}"`);
        code = 1;
    }
}
try { rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch { /* browser helpers may still hold files */ }
console.log(code === 0 ? 'persist_smoke: OK' : 'persist_smoke: FAILED');
process.exit(code);
