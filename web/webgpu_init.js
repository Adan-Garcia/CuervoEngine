// WebGPU device request for the web build (--pre-js; DESIGN.md 4.4 "Clarified
// (Milestone 10.3)", docs/WEB.md "WebGPU").
//
// The browser's adapter and device requests are promises, which a WASM main()
// can't wait for without JSPI, so the page makes them before main runs (a run
// dependency) and hands the device to emdawnwebgpu as
// Module.preinitializedWebGPUDevice. The player then defaults to WebGPU
// (rhi::default_backend); with no navigator.gpu, no adapter, a failed request
// or `?args=--backend gl` there is no device and it runs on WebGL2 instead.
// One build serves both, in any browser.
(function () {
    // Worker threads (CUERVO_WEB_THREADS) load this file too; only the page
    // renders.
    if (typeof ENVIRONMENT_IS_PTHREAD !== 'undefined' && ENVIRONMENT_IS_PTHREAD) return;
    var args = Module['arguments'] || [];
    var backend = args.indexOf('--backend');
    if (backend >= 0 && args[backend + 1] !== 'webgpu' && args[backend + 1] !== 'wgpu') return;
    if (typeof navigator === 'undefined' || !navigator.gpu) {
        console.log('[web] no WebGPU in this browser: using WebGL2');
        return;
    }
    Module['preRun'] = [].concat(Module['preRun'] || [], function () {
        addRunDependency('cuervo-webgpu');
        var done = function () { removeRunDependency('cuervo-webgpu'); };
        var fallBack = function (why) {
            console.log('[web] WebGPU unavailable (' + why + '): using WebGL2');
            done();
        };
        // powerPreference is a hint, yet some browsers (headless Chrome on
        // Linux) answer 'high-performance' with no adapter at all: ask again
        // without it before giving up.
        navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }).then(function (adapter) {
            return adapter || navigator.gpu.requestAdapter();
        }).then(function (adapter) {
            if (!adapter) return fallBack('no adapter');
            return adapter.requestDevice({ label: 'cuervo' }).then(function (device) {
                // The engine's own error callback can't be attached to a
                // device made here; the console prefix matches its log.
                device.onuncapturederror = function (e) {
                    console.error('[error] WebGPU ' + (e.error && e.error.constructor ? e.error.constructor.name : 'error') +
                                  ': ' + (e.error ? e.error.message : ''));
                };
                device.lost.then(function (info) {
                    if (info.reason !== 'destroyed') console.error('[error] WebGPU device lost: ' + info.message);
                });
                Module['preinitializedWebGPUDevice'] = device;
                done();
            });
        }).catch(function (e) { fallBack(String(e)); });
    });
})();
