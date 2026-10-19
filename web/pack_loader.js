// --pre-js for the pack-loading web runtime (CUERVO_WEB_PACK=fetch,
// cmake/Web.cmake; docs/EXPORT.md "Web"): before main() runs, fetch the
// exported game's archive next to the page (or Module.cuervoPackUrl) into
// MEMFS at /game.pak, the path the runtime opens. One runtime build serves
// every game: `cuervo-tools export --platform web` only copies it and writes
// game.pak. The marker string "cuervo-pack" identifies this build.
Module['preRun'] = [].concat(Module['preRun'] || []);
Module['preRun'].push(function () {
  var url = Module['cuervoPackUrl'] || 'game.pak';
  addRunDependency('cuervo-pack');
  fetch(url).then(function (response) {
    if (!response.ok) throw new Error('HTTP ' + response.status);
    return response.arrayBuffer();
  }).then(function (buffer) {
    FS.writeFile('/game.pak', new Uint8Array(buffer));
    removeRunDependency('cuervo-pack');
  }).catch(function (e) {
    abort('cuervo: cannot load the game pack ' + url + ': ' + e);
  });
});
