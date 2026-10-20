// Search box (tools/build_site.py): loads assets/search-index.json on first
// focus and filters it in the page. Entries are [title, url, page, text].
(function () {
  "use strict";
  var input = document.getElementById("search-input");
  var list = document.getElementById("search-results");
  if (!input || !list) return;
  var base = window.CUERVO_SITE_ROOT || "./";
  var index = null, loading = false, sel = -1, shown = [];

  function load(cb) {
    if (index) return cb();
    if (loading) return;
    loading = true;
    fetch(base + "assets/search-index.json").then(function (r) { return r.json(); }).then(function (j) {
      index = j.map(function (e) {
        return { t: e[0], u: e[1], p: e[2], x: e[3], tl: e[0].toLowerCase(), xl: (e[2] + " " + e[3]).toLowerCase() };
      });
      cb();
    }).catch(function () { loading = false; });
  }

  function score(e, words) {
    var s = 0;
    for (var i = 0; i < words.length; i++) {
      var w = words[i], inTitle = e.tl.indexOf(w), inText = e.xl.indexOf(w);
      if (inTitle < 0 && inText < 0) return 0;
      if (e.tl === w) s += 60;
      else if (inTitle === 0) s += 30;
      else if (inTitle > 0) s += 14;
      if (inText >= 0) s += 2;
      if (e.t.indexOf(".") > 0 && e.tl.split(".").pop() === w) s += 20;
    }
    return s - Math.min(e.tl.length, 60) / 60;
  }

  function esc(s) {
    return s.replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; });
  }
  function mark(text, words) {
    var out = esc(text);
    words.forEach(function (w) {
      if (w.length < 2) return;
      out = out.replace(new RegExp("(" + w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + ")", "ig"), "<mark>$1</mark>");
    });
    return out;
  }

  function snippet(e, words) {
    var xl = e.x.toLowerCase(), at = -1;
    for (var i = 0; i < words.length && at < 0; i++) at = xl.indexOf(words[i]);
    var from = Math.max(0, at - 30);
    return (from > 0 ? "…" : "") + e.x.slice(from, from + 110);
  }

  function render() {
    var q = input.value.trim().toLowerCase();
    if (!q || !index) { list.hidden = true; return; }
    var words = q.split(/\s+/);
    var hits = [];
    for (var i = 0; i < index.length; i++) {
      var s = score(index[i], words);
      if (s > 0) hits.push([s, index[i]]);
    }
    hits.sort(function (a, b) { return b[0] - a[0]; });
    shown = hits.slice(0, 12).map(function (h) { return h[1]; });
    sel = shown.length ? 0 : -1;
    if (!shown.length) {
      list.innerHTML = '<li class="none">No results for “' + esc(input.value) + '”.</li>';
    } else {
      list.innerHTML = shown.map(function (e, i) {
        return '<li role="option" aria-selected="' + (i === sel) + '"><a href="' + base + e.u + '">' +
          '<span class="r-title">' + mark(e.t, words) + '</span><span class="r-where">' + esc(e.p) + '</span>' +
          '<span class="r-text">' + mark(snippet(e, words), words) + '</span></a></li>';
      }).join("");
    }
    list.hidden = false;
  }

  function move(d) {
    if (!shown.length) return;
    sel = (sel + d + shown.length) % shown.length;
    Array.prototype.forEach.call(list.children, function (li, i) { li.setAttribute("aria-selected", i === sel); });
    var el = list.children[sel];
    if (el && el.scrollIntoView) el.scrollIntoView({ block: "nearest" });
  }

  input.addEventListener("focus", function () { load(render); });
  input.addEventListener("input", function () { load(render); });
  input.addEventListener("keydown", function (e) {
    if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
    else if (e.key === "ArrowUp") { e.preventDefault(); move(-1); }
    else if (e.key === "Enter" && sel >= 0) { window.location.href = base + shown[sel].u; }
    else if (e.key === "Escape") { list.hidden = true; input.blur(); }
  });
  document.addEventListener("click", function (e) { if (!e.target.closest(".search")) list.hidden = true; });
  document.addEventListener("keydown", function (e) {
    var tag = (document.activeElement && document.activeElement.tagName) || "";
    if (e.key === "/" && tag !== "INPUT" && tag !== "TEXTAREA") { e.preventDefault(); input.focus(); input.select(); }
  });
})();
