// --pre-js: persistent saves for the web build (docs/WEB.md "Persistent saves").
// /persist is an IDBFS mount. Before main() it is filled from IndexedDB (a run
// dependency, so the game starts after the load); save writes land in the
// in-memory file system synchronously and cuervoPersistFlush() writes them
// back, coalesced to one syncfs at a time. If IndexedDB is unavailable
// (private windows) the mount stays an ordinary in-memory directory.
Module['preRun'] = [].concat(Module['preRun'] || []);
Module['preRun'].push(function () {
  var syncing = false, again = false;
  try {
    FS.mkdir('/persist');
    FS.mount(IDBFS, {}, '/persist');
  } catch (e) {
    console.warn('cuervo: persistent saves unavailable: ' + e);
    return;
  }
  Module['cuervoPersistFlush'] = function () {
    if (syncing) { again = true; return; }
    syncing = true;
    FS.syncfs(false, function (err) {
      if (err) console.warn('cuervo: save flush failed: ' + err);
      syncing = false;
      if (again) { again = false; Module['cuervoPersistFlush'](); }
    });
  };
  addRunDependency('cuervo-persist');
  FS.syncfs(true, function (err) {
    if (err) console.warn('cuervo: save load failed: ' + err);
    removeRunDependency('cuervo-persist');
  });
});
