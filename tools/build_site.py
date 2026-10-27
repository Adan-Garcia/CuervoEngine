#!/usr/bin/env python3
"""Builds the Cuervo Engine documentation site (DESIGN.md 4.16, milestone 8.4).

    python3 -I tools/build_site.py -o build/site \
        --font display=<Outfit-Medium.ttf> --font ui=<Figtree-Regular.ttf> \
        --font ui-bold=<Figtree-SemiBold.ttf> --font mono=<JetBrainsMono-Regular.ttf>

A static site (plain HTML, one CSS file, a little vanilla JS, a JSON search
index) generated from the repository's Markdown, with the 8.1 brand: colors
from assets/brand/tokens.json in light and dark (prefers-color-scheme), the
brand fonts shipped with the site under their OFL licenses, and the Cuervo
Mark and wordmark. Standard library only: no npm, no pip.

The documents are the source of truth. This script only decides how they are
cut into pages (see `build_pages`), rewrites relative links between them to
site pages, and checks afterwards that every internal link and fragment in
the output resolves (`--no-check` skips that; the ctest `docs_site` runs it).
"""

import argparse
import html
import json
import os
import posixpath
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "site"))
sys.path.insert(0, str(HERE))

import highlight  # noqa: E402
import mdlite  # noqa: E402

ROOT = HERE.parent

# Top navigation: (label, section id, landing page).
SECTIONS = [
    ("Manual", "manual", "manual/getting-started.html"),
    ("Pluma", "pluma", "pluma/guide.html"),
    ("Tutorial", "tutorial", "tutorial/index.html"),
    ("API", "api", "api/index.html"),
    ("Design", "design", "design/index.html"),
    ("About", "about", "about/license.html"),
]


class Source:
    """A Markdown file: its lines, and its headings with GitHub-style ids."""

    def __init__(self, path, text=None):
        self.path = path  # repo-relative, '/'-separated
        if text is None:
            text = (ROOT / path).read_text(encoding="utf-8")
        self.lines = text.split("\n")
        self.headings = []  # (line index, level, text, slug)
        self.slug_at = {}
        used = {}
        fence = None
        for i, line in enumerate(self.lines):
            m = mdlite.FENCE_RE.match(line)
            if m:
                token = m.group(2)[0]
                if fence is None:
                    fence = token
                elif fence == token:
                    fence = None
                continue
            if fence:
                continue
            h = mdlite.HEADING_RE.match(line)
            if h:
                base = mdlite.slugify(h.group(2))
                n = used.get(base, 0)
                used[base] = n + 1
                slug = base if n == 0 else f"{base}-{n}"
                self.headings.append((i, len(h.group(1)), h.group(2), slug))
                self.slug_at[i] = slug
        self.anchor_url = {}  # slug -> page url, filled when pages are laid out
        self.first_url = None

    def find(self, level, prefix, start=0):
        for idx, (i, lvl, text, _) in enumerate(self.headings):
            if lvl == level and i >= start and mdlite.strip_markup(text).startswith(prefix):
                return idx
        raise KeyError(f"{self.path}: no level-{level} heading starting with {prefix!r}")

    def section(self, level, prefix, until_prefix=None, until_level=None):
        """Line range [lo, hi) from a heading to the next heading of the same or a higher level."""
        k = self.find(level, prefix)
        lo = self.headings[k][0]
        hi = len(self.lines)
        for i, lvl, text, _ in self.headings[k + 1 :]:
            if lvl <= level:
                hi = i
                break
        if until_prefix:  # extend over the following sections, up to (not including) this heading
            j = self.find(until_level or level, until_prefix, start=lo + 1)
            hi = self.headings[j][0]
        return lo, hi

    def children(self, lo, hi, level):
        """Line ranges of the level-`level` headings inside [lo, hi)."""
        starts = [i for (i, lvl, _, _) in self.headings if lo <= i < hi and lvl == level]
        return [(s, starts[k + 1] if k + 1 < len(starts) else hi) for k, s in enumerate(starts)]

    def title_at(self, line):
        for i, lvl, text, _ in self.headings:
            if i == line:
                return mdlite.strip_markup(text)
        return ""


class Page:
    def __init__(self, url, title, section, group="", src=None, lo=0, hi=None, lines=None, api=False, h1=None, plain=None, body=""):
        self.url = url
        self.title = title
        self.section = section
        self.group = group
        self.src = src
        self.lo = lo
        self.hi = len(src.lines) if (src is not None and hi is None) else hi
        self.lines = lines  # virtual Markdown (index pages), instead of a source range
        self.api = api
        self.h1 = h1  # replaces the first heading's text
        self.plain = plain  # preformatted text instead of Markdown (license files)
        self.seq = None  # pages for previous/next links (set by the section)
        self.body = body
        self.toc = []
        self.members = []
        self.entries = []
        self.description = ""

    def chunk(self):
        return self.lines if self.lines is not None else self.src.lines[self.lo : self.hi]


# ---------------------------------------------------------------- site layout


def build_pages(opts):
    """Cuts the repository's documents into pages. Returns (pages, docs, file_targets)."""
    pages = []
    docs = {}

    def doc(path):
        if path not in docs:
            docs[path] = Source(path)
        return docs[path]

    def add(*a, **k):
        p = Page(*a, **k)
        pages.append(p)
        return p

    # ---- Manual
    g = doc("docs/GUIDE.md")
    lo1, _ = g.section(2, "1.")
    _, hi3 = g.section(2, "3.")
    add("manual/getting-started.html", "Getting started", "manual", "Start", g, lo1, hi3)
    add("manual/status.html", "Support status", "manual", "Start", doc("docs/STATUS.md"))
    add("manual/roadmap.html", "Roadmap", "manual", "Start", doc("docs/ROADMAP.md"))
    add("manual/hot-reload-and-editor.html", "Hot reload and the editor", "manual", "Start", g, *g.section(2, "6.", "8.", 2))
    add("manual/debugging.html", "Debugging and profiling", "manual", "Start", g, *g.section(2, "8."))
    lo9, _ = g.section(2, "9.")
    _, hi10 = g.section(2, "10.")
    add("manual/shipping.html", "Shipping your game", "manual", "Start", g, lo9, hi10)
    add("manual/editor.html", "The editor", "manual", "Reference", doc("editor/README.md"))
    add("manual/scenes.html", "Scene files", "manual", "Reference", doc("docs/SCENES.md"))
    add("manual/replays.html", "Replays", "manual", "Reference", doc("docs/REPLAYS.md"))
    add("manual/export.html", "Packing and export", "manual", "Reference", doc("docs/EXPORT.md"))
    add("manual/packages.html", "Packages", "manual", "Reference", doc("docs/PACKAGES.md"))
    add("manual/plugins.html", "Plugins, FFI and embedding", "manual", "Reference", doc("docs/PLUGINS.md"))
    add("manual/web.html", "The web runtime", "manual", "Reference", doc("docs/WEB.md"))
    add("manual/windows.html", "Windows", "manual", "Reference", doc("docs/WINDOWS.md"))
    add("manual/performance.html", "Performance baselines", "manual", "Reference", doc("docs/PERFORMANCE.md"))
    add("manual/vscode.html", "VS Code extension", "manual", "Reference", doc("tools/vscode-pluma/README.md"))

    # ---- Pluma
    pg = doc("docs/PLUMA_GUIDE.md")
    chapters = pg.children(0, len(pg.lines), 2)
    intro_hi = chapters[0][0]
    p = add("pluma/guide.html", "The Pluma guide", "pluma", "Guide", pg, 0, intro_hi, lines=[])
    contents = ["## Chapters", ""]
    for lo, hi in chapters:
        title = re.sub(r"^\d+\.\s+", "", pg.title_at(lo))
        slug = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
        add(f"pluma/guide/{slug}.html", title, "pluma", "Guide", pg, lo, hi)
        contents.append(f"{len(contents) - 1}. [{title}](guide/{slug}.html)")
    p.lines = pg.lines[0:intro_hi] + contents
    lo4, hi4 = g.section(2, "4.")
    add("pluma/five-minutes.html", "Pluma in five minutes", "pluma", "Start", g, lo4, hi4, h1="Pluma in five minutes")
    pages.insert(pages.index(p), pages.pop())  # the quick tour before the guide
    lang = doc("docs/LANGUAGE.md")
    secs = lang.children(0, len(lang.lines), 2)
    spec_pages = []
    for lo, hi in secs:
        title = re.sub(r"^\d+\.\s+", "", lang.title_at(lo)).strip()
        num = re.match(r"^(\d+)\.", lang.title_at(lo))
        slug = f"{num.group(1)}" if num else "x"
        spec_pages.append(add(f"pluma/spec/{slug}.html", f"{num.group(1)}. {title}" if num else title, "pluma", "Language reference", lang, lo, hi, h1=lang.title_at(lo)))
    index_lines = lang.lines[0 : secs[0][0]] + ["", "## Sections", ""]
    for sp in spec_pages:
        index_lines.append(f"- [{sp.title}]({posixpath.relpath(sp.url, 'pluma/spec')})")
    # The index page links with site-relative urls handled by the resolver (see resolve_site).
    idx_page = add("pluma/spec/index.html", "Language reference", "pluma", "Language reference", lang, 0, secs[0][0], lines=index_lines)
    pages.insert(pages.index(spec_pages[0]), pages.pop())

    # ---- Tutorial
    lo5, hi5 = g.section(2, "5.")
    steps = g.children(lo5, hi5, 3)
    add("tutorial/index.html", "Tutorial: Starfall", "tutorial", "Starfall", g, lo5, steps[0][0], h1="Tutorial: Starfall")
    for k, (lo, hi) in enumerate(steps, 1):
        title = g.title_at(lo)
        add(f"tutorial/step-{k}.html", title, "tutorial", "Starfall", g, lo, hi)
    tut = ROOT / "examples" / "tutorial"
    files = ["project.toml"] + sorted(f"scripts/{f.name}" for f in (tut / "scripts").glob("*.plm"))
    src_lines = [
        "# The finished game",
        "",
        "Every file of `examples/tutorial/`, as the repository has it. Run it with `cuervo --game examples/tutorial`.",
        "The tutorial pages above build these files step by step; a test fails if they ever disagree.",
        "",
    ]
    for f in files:
        lang_name = "pluma" if f.endswith(".plm") else "toml"
        src_lines += [f"## {f}", "", f"```{lang_name}", (tut / f).read_text(encoding="utf-8").rstrip("\n"), "```", ""]
    scene = tut / "scenes" / "game_over.scene"
    if scene.exists():
        src_lines += ["## scenes/game_over.scene", "", "A binary scene file (docs/SCENES.md), built from a text description with `cuervo-tools scene build`.", ""]
    tsrc = Source("examples/tutorial/index.md", "\n".join(src_lines))
    docs["examples/tutorial/index.md"] = tsrc
    add("tutorial/source.html", "The finished game", "tutorial", "Starfall", tsrc, 0, len(tsrc.lines))

    # ---- API
    api = doc("docs/API.md")
    contents_i = api.headings[api.find(2, "Contents")][0]
    file_secs = [(i, i2) for (i, i2) in api.children(0, len(api.lines), 2) if api.title_at(i) != "Contents"]
    api_files = []  # (file page, [type pages])
    # The generated header comment spans several lines; drop it whole.
    text = "\n".join(api.lines[api.headings[0][0] + 1 : contents_i])
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    index_md = text.strip().split("\n") + ["", "## Files", ""]
    type_pages_all = []
    for lo, hi in file_secs:
        fname = api.title_at(lo)
        fslug = fname.replace(".plm", "")
        types = [(a, b) for (a, b) in api.children(lo, hi, 3) if api.title_at(a) != "Constants and functions"]
        file_hi = types[0][0] if types else hi
        fpage = add(f"api/files/{fslug}.html", fname, "api", fname, api, lo, file_hi, api=True)
        tpages = []
        for a, b in types:
            name = api.title_at(a)
            tp = add(f"api/{name}.html", name, "api", fname, api, a, b, api=True)
            tpages.append(tp)
        fpage.types = tpages
        for tp in tpages:
            tp.seq = tpages
            tp.file_page = fpage
        api_files.append(fpage)
        type_pages_all += tpages
    ip = add("api/index.html", "API reference", "api", "Overview", api, 0, 0, lines=index_md)
    ip.api_files = api_files

    # ---- Design
    add("design/index.html", "Design", "design", "Design", doc("docs/DESIGN.md"))

    # ---- About
    add("about/license.html", "License", "about", "Legal", doc("LICENSE.md"))
    add("about/third-party.html", "Third-party notices", "about", "Legal", doc("THIRD_PARTY_NOTICES.md"))
    lic_dir = ROOT / "third_party" / "licenses"
    lic_pages = []
    for f in sorted(lic_dir.glob("*.txt"), key=lambda x: x.name.lower()):
        lic_pages.append(add(f"about/licenses/{f.stem}.html", f.stem, "about", "License texts", plain=f.read_text(encoding="utf-8", errors="replace")))
    return pages, docs


def lay_out(pages, docs):
    """Maps every heading of every source document to the page that shows it."""
    for p in pages:
        if p.src is None or p.lines is not None:
            continue
        s = p.src
        if s.first_url is None:
            s.first_url = p.url
        for i, _, _, slug in s.headings:
            if p.lo <= i < p.hi:
                s.anchor_url.setdefault(slug, f"{p.url}#{slug}")
    for s in docs.values():
        if s.first_url is None:
            for p in pages:
                if p.src is s:
                    s.first_url = p.url
                    break


def file_targets(pages):
    """Repository paths that are not Markdown pages but have a page: license texts, the tutorial."""
    t = {"third_party/licenses/": "about/third-party.html", "examples/tutorial": "tutorial/source.html", "examples/tutorial/": "tutorial/source.html"}
    for p in pages:
        if p.url.startswith("about/licenses/"):
            t[f"third_party/licenses/{p.title}.txt"] = p.url
    tut = ROOT / "examples" / "tutorial"
    for f in ["project.toml"] + [f"scripts/{x.name}" for x in (tut / "scripts").glob("*.plm")]:
        slug = mdlite.slugify(f)
        t[f"examples/tutorial/{f}"] = f"tutorial/source.html#{slug}"
    t["examples/tutorial/scenes/game_over.scene"] = "tutorial/source.html"
    t["README.md"] = "index.html"
    return {k: v for k, v in t.items() if v}


# ---------------------------------------------------------------- rendering


def make_resolver(page, docs, targets):
    base = page.src.path if page.src is not None else "docs/GUIDE.md"
    if page.url.startswith(("pluma/spec/index", "api/index")):
        base = page.src.path

    def resolve(href):
        path, _, frag = href.partition("#")
        if page.lines is not None and path.endswith(".html"):
            return "@@SITE/" + posixpath.normpath(posixpath.join(posixpath.dirname(page.url), path))
        if not path:
            target = base
        else:
            target = posixpath.normpath(posixpath.join(posixpath.dirname(base), path))
            if path.endswith("/"):
                target += "/"
        if target in docs:
            d = docs[target]
            if frag:
                url = d.anchor_url.get(frag)
                if url is None:
                    return None
                return "@@SITE/" + url
            return "@@SITE/" + (d.first_url or "")
        if target in targets:
            url = targets[target]
            if frag and "#" not in url:
                url += "#" + frag
            return "@@SITE/" + url
        return None

    return resolve


def render_page(page, docs, targets):
    if page.plain is not None:
        page.body = f'<h1>{html.escape(page.title)} license</h1>\n<pre class="plain">{html.escape(page.plain)}</pre>'
        page.description = f"License text of {page.title}"
        return []
    ctx = mdlite.Context(resolve=make_resolver(page, docs, targets), api=page.api)
    lines = page.chunk()
    if page.lines is None:
        ctx.slug_at = page.src.slug_at
        ctx.base_line = page.lo
    first = next((i for i, l in enumerate(lines) if l.strip()), None)
    first_heading = mdlite.HEADING_RE.match(lines[first]) if first is not None else None
    prefix = ""
    if page.lines is None and first_heading:
        level = len(first_heading.group(1))
        others = [1 for (i, lvl, _, _) in page.src.headings if page.lo + first < i < page.hi and lvl <= level]
        if not others:
            ctx.level_shift = level - 1
            text = page.h1 or re.sub(r"^\d+\.\s+", "", first_heading.group(2))
            ctx.h1_text = text
        else:
            prefix = f"<h1>{html.escape(page.h1 or page.title)}</h1>\n"
    elif page.lines is not None:
        # Virtual pages: their first heading, if any, is the title.
        fh = first_heading
        if fh:
            ctx.level_shift = len(fh.group(1)) - 1
        else:
            prefix = f"<h1>{html.escape(page.title)}</h1>\n"
    else:
        prefix = f"<h1>{html.escape(page.h1 or page.title)}</h1>\n"
    body = mdlite.render(lines, ctx)
    page.body = prefix + body
    page.toc = [(lvl, text, hid) for (lvl, text, hid) in ctx.headings if 2 <= lvl <= 3]
    page.members = ctx.members
    page.warnings = ctx.warnings
    return ctx.warnings


def add_api_extras(pages):
    """Type summaries on file pages and the file/type index on the API landing page."""
    by_url = {p.url: p for p in pages}

    def summary(tp):
        m = re.search(r"</div>\s*<p>(.*?)</p>", tp.body, re.S)
        if not m:
            return ""
        text = re.sub(r"<[^>]+>", "", m.group(1))
        text = html.unescape(text).strip().replace("\n", " ")
        cut = re.search(r"(?<=[.!?])\s", text)
        return (text[: cut.start()] if cut else text)[:200]

    for p in pages:
        if getattr(p, "types", None):
            rows = "".join(
                f'<tr><td><a href="@@SITE/{t.url}"><code>{html.escape(t.title)}</code></a></td><td>{html.escape(summary(t))}</td></tr>' for t in p.types
            )
            p.body += f'\n<h2 id="types">Types</h2>\n<div class="table-wrap"><table class="summary"><tbody>{rows}</tbody></table></div>'
            p.toc.append((2, "Types", "types"))
    ip = by_url["api/index.html"]
    blocks = []
    for fp in ip.api_files:
        names = " ".join(f'<a href="@@SITE/{t.url}"><code>{html.escape(t.title)}</code></a>' for t in fp.types)
        blocks.append(f'<div class="card"><h3><a href="@@SITE/{fp.url}">{html.escape(fp.title)}</a></h3><p class="chips">{names}</p></div>')
    az = sorted((t for fp in ip.api_files for t in fp.types), key=lambda t: t.title.lower())
    letters = {}
    for t in az:
        letters.setdefault(t.title[0].upper(), []).append(t)
    idx = "".join(
        f'<div class="az"><span class="az-letter">{l}</span>' + " ".join(f'<a href="@@SITE/{t.url}"><code>{html.escape(t.title)}</code></a>' for t in ts) + "</div>"
        for l, ts in sorted(letters.items())
    )
    ip.body += f'\n<div class="cards">{"".join(blocks)}</div>\n<h2 id="all-types">All types, A to Z</h2>\n<div class="az-list">{idx}</div>'
    ip.toc = [(2, "Files", "files"), (2, "All types, A to Z", "all-types")]


# ---------------------------------------------------------------- chrome


def css_tokens(tokens):
    flat = tokens["color"]

    def val(v):
        m = re.fullmatch(r"\{color\.(\w+)\.(\w+)\}", v)
        if m:
            return flat[m.group(1)][m.group(2)]["$value"]
        return v

    def theme(name):
        return {k: val(v["$value"]) for k, v in tokens["semantic"][name].items() if isinstance(v, dict) and "$value" in v}

    c = lambda g, s: flat[g][s]["$value"]  # noqa: E731
    syntax = {
        "light": {"kw": c("ember", "700"), "decl": c("ember", "700"), "ty": c("steel", "600"), "fn": c("graphite", "900"), "call": c("steel", "700"),
                  "str": c("sage", "700"), "num": c("amber", "700"), "cm": c("graphite", "500"), "at": c("amber", "700"), "sym": c("sage", "600"),
                  "const": c("amber", "700"), "self": c("ember", "600"), "cn": c("amber", "700")},
        "dark": {"kw": c("ember", "300"), "decl": c("ember", "300"), "ty": c("steel", "300"), "fn": c("graphite", "50"), "call": c("steel", "200"),
                 "str": c("sage", "300"), "num": c("amber", "400"), "cm": c("graphite", "400"), "at": c("amber", "400"), "sym": c("sage", "300"),
                 "const": c("amber", "400"), "self": c("ember", "400"), "cn": c("amber", "400")},
    }

    def block(name):
        lines = [f"  --{k}: {v};" for k, v in theme(name).items()]
        lines += [f"  --t-{k}: {v};" for k, v in syntax[name].items()]
        lines.append(f"  color-scheme: {name};")
        return "\n".join(lines)

    fam = tokens["fontFamily"]

    def stack(names):
        return ", ".join(f"'{n}'" if " " in n else n for n in names)

    out = [":root {", block("light"),
           f"  --font-display: {stack(fam['display']['$value'])};",
           f"  --font-body: {stack(fam['body']['$value'])};",
           f"  --font-mono: {stack(fam['mono']['$value'])};"]
    for k, v in tokens.get("radius", {}).items():
        out.append(f"  --radius-{k}: {v['$value']};")
    for k, v in tokens.get("shadow", {}).items():
        out.append(f"  --shadow-{k}: {v['$value']};")
    out.append("}")
    out.append("@media (prefers-color-scheme: dark) {\n  :root:not([data-theme=\"light\"]) {\n" + block("dark") + "\n  }\n}")
    out.append(":root[data-theme=\"dark\"] {\n" + block("dark") + "\n}")
    return "\n".join(out)


THEME_BOOT = "try{var t=localStorage.getItem('cuervo-docs-theme');if(t)document.documentElement.setAttribute('data-theme',t)}catch(e){}"


def nav_html(page, pages_by_section):
    top = []
    for label, sec, landing in SECTIONS:
        cur = ' aria-current="page"' if sec == page.section else ""
        top.append(f'<a href="@@SITE/{landing}"{cur}>{label}</a>')
    return "".join(top)


def sidebar_html(page, pages_by_section):
    members = pages_by_section.get(page.section, [])
    out = []
    if page.section == "api":
        # Files, with the current file's types expanded.
        cur_file = getattr(page, "file_page", page)
        out.append(f'<a class="side-top" href="@@SITE/api/index.html"{" aria-current=\"page\"" if page.url == "api/index.html" else ""}>Overview</a>')
        for fp in [p for p in members if p.url.startswith("api/files/")]:
            open_attr = " open" if fp is cur_file else ""
            cur = ' aria-current="page"' if fp is page else ""
            kids = "".join(
                f'<a href="@@SITE/{t.url}"{" aria-current=\"page\"" if t is page else ""}>{html.escape(t.title)}</a>' for t in fp.types
            )
            out.append(f'<details{open_attr}><summary><a href="@@SITE/{fp.url}"{cur}>{html.escape(fp.title)}</a></summary><div class="side-kids">{kids}</div></details>')
        return "".join(out)
    group = None
    for p in members:
        if p.group != group:
            group = p.group
            if group:
                out.append(f'<div class="side-group">{html.escape(group)}</div>')
        if p.section == "about" and p.url.startswith("about/licenses/"):
            cur = ' aria-current="page"' if p is page else ""
            out.append(f'<a class="small" href="@@SITE/{p.url}"{cur}>{html.escape(p.title)}</a>')
            continue
        cur = ' aria-current="page"' if p is page else ""
        out.append(f'<a href="@@SITE/{p.url}"{cur}>{html.escape(p.title)}</a>')
    return "".join(out)


def prev_next(page, pages_by_section):
    seq = page.seq
    if seq is None:
        seq = [p for p in pages_by_section.get(page.section, []) if not p.url.startswith(("api/", "about/licenses/"))]
        if page.section == "api" or page.url.startswith("about/licenses/"):
            return ""
    if page not in seq:
        return ""
    i = seq.index(page)
    bits = []
    if i > 0:
        bits.append(f'<a class="pn prev" href="@@SITE/{seq[i - 1].url}"><span>Previous</span>{html.escape(seq[i - 1].title)}</a>')
    else:
        bits.append("<span></span>")
    if i + 1 < len(seq):
        bits.append(f'<a class="pn next" href="@@SITE/{seq[i + 1].url}"><span>Next</span>{html.escape(seq[i + 1].title)}</a>')
    return f'<nav class="prev-next" aria-label="Pages">{"".join(bits)}</nav>'


def toc_html(page):
    items = []
    if page.api and page.members:
        items = [(3, name, hid) for name, hid in page.members]
        head = "Members"
    else:
        items = page.toc
        head = "On this page"
    if len(items) < 2:
        return ""
    lis = "".join(f'<li class="l{lvl}"><a href="#{hid}">{html.escape(text)}</a></li>' for lvl, text, hid in items)
    return f'<aside class="toc" aria-label="{head}"><div class="toc-head">{head}</div><ul>{lis}</ul></aside>'


def breadcrumbs(page):
    label = next(l for (l, s, _) in SECTIONS if s == page.section)
    landing = next(u for (l, s, u) in SECTIONS if s == page.section)
    return f'<div class="crumbs"><a href="@@SITE/{landing}">{label}</a>' + (f'<span>{html.escape(page.group)}</span>' if page.group and page.section in ("manual", "pluma", "api") else "") + "</div>"


def page_html(page, pages_by_section, build_id):
    depth = page.url.count("/")
    title = "Cuervo Engine documentation" if page.url == "index.html" else f"{page.title} - Cuervo Engine documentation"
    desc = html.escape(page.description or f"{page.title}: Cuervo Engine documentation.")
    home_cls = " home" if page.url == "index.html" else ""
    side = sidebar_html(page, pages_by_section) if page.section else ""
    main = page.body
    toc = toc_html(page) if page.section else ""
    pn = prev_next(page, pages_by_section) if page.section else ""
    crumbs = breadcrumbs(page) if page.section else ""
    nav = nav_html(page, pages_by_section)
    sidebar = f'<nav class="sidebar" id="sidebar" aria-label="Section pages">{side}</nav>' if side else ""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<meta name="description" content="{desc}">
<meta name="color-scheme" content="light dark">
<link rel="icon" href="@@SITE/assets/brand/favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="@@SITE/assets/site.css">
<script>{THEME_BOOT}</script>
</head>
<body class="{'has-side' if side else 'no-side'}{home_cls}">
<a class="skip" href="#content">Skip to content</a>
<header class="topbar">
  <button class="menu-btn" type="button" aria-label="Toggle navigation" aria-controls="sidebar">Menu</button>
  <a class="brand" href="@@SITE/index.html" aria-label="Cuervo Engine documentation, home">
    <img class="mark mark-light" src="@@SITE/assets/brand/mark-ember.svg" alt="" width="28" height="28">
    <img class="mark mark-dark" src="@@SITE/assets/brand/mark-ember-dark.svg" alt="" width="28" height="28">
    <img class="wordmark wm-light" src="@@SITE/assets/brand/wordmark-light.svg" alt="Cuervo" width="86" height="16">
    <img class="wordmark wm-dark" src="@@SITE/assets/brand/wordmark-dark.svg" alt="" width="86" height="16"><span class="brand-docs">Docs</span>
  </a>
  <nav class="topnav" aria-label="Sections">{nav}</nav>
  <div class="search" role="search">
    <input id="search-input" type="search" placeholder="Search docs" aria-label="Search the documentation" autocomplete="off" spellcheck="false">
    <kbd>/</kbd>
    <ul id="search-results" class="results" role="listbox" hidden></ul>
  </div>
  <button class="theme-btn" type="button" aria-label="Switch between light and dark">Theme</button>
</header>
<div class="layout">
{sidebar}
<main id="content" class="content">
{crumbs}
<article class="doc">
{main}
</article>
{pn}
<footer class="foot">Cuervo Engine documentation. Generated from the repository's Markdown by <code>tools/build_site.py</code>; build {html.escape(build_id)}. <a href="@@SITE/about/license.html">License</a> · <a href="@@SITE/about/third-party.html">Third-party notices</a></footer>
</main>
{toc}
</div>
<script src="@@SITE/assets/search.js" defer></script>
<script src="@@SITE/assets/site.js" defer></script>
<script>window.CUERVO_SITE_ROOT="@@ROOT";</script>
</body>
</html>
"""


def home_html(pages):
    by_url = {p.url: p for p in pages}
    hero = """
<section class="hero">
  <div class="hero-mark">
    <img class="mark-light" src="@@SITE/assets/brand/mark-ember.svg" alt="The Cuervo Mark" width="168" height="168">
    <img class="mark-dark" src="@@SITE/assets/brand/mark-ember-dark.svg" alt="The Cuervo Mark" width="168" height="168">
  </div>
  <div class="hero-text">
    <p class="eyebrow">Documentation</p>
    <h1>Make 2D games that play the same every time.</h1>
    <p class="lead">Cuervo Engine is a vector-first, deterministic 2D engine with its own statically typed scripting language, <strong>Pluma</strong>. Same inputs, same game, on Vulkan, OpenGL and the web.</p>
    <p class="cta"><a class="btn primary" href="@@SITE/manual/getting-started.html">Get started</a> <a class="btn" href="@@SITE/tutorial/index.html">Build Starfall</a> <a class="btn" href="@@SITE/api/index.html">API reference</a></p>
  </div>
</section>
"""
    cards = [
        ("Manual", "manual/getting-started.html", "Install, build and run; the editor, scenes, replays, debugging, and packing a game for Linux and the web. Start with the support status."),
        ("Pluma", "pluma/guide.html", "A guide to the scripting language, Pluma in five minutes, and the full language reference."),
        ("Tutorial", "tutorial/index.html", "Build Starfall, a small complete game: movement, particles, collisions, a HUD and a game-over menu."),
        ("API reference", "api/index.html", "Every class, field, method and signal scripts can use, generated from the engine itself."),
        ("Design", "design/index.html", "Architecture and the decisions behind it: determinism, rendering, scripting, the editor."),
        ("About", "about/license.html", "The engine license and the third-party notices."),
    ]
    c = "".join(f'<a class="card link-card" href="@@SITE/{u}"><h3>{t}</h3><p>{d}</p></a>' for t, u, d in cards)
    # Hand-picked rather than cut from a file, so it is always a whole, small class.
    sample = """class Player extends Node2D {
    var speed = 420.0
    var invulnerable = false

    fn update(dt: float) {
        position += Input.axis(#move) * speed * dt
    }

    fn hurt() {
        if invulnerable { return }
        start(blink())             // runs alongside the game
    }

    fn blink() {
        invulnerable = true
        for i in 0..10 {
            visible = !visible
            await wait(0.1)        // game time, so replays match
        }
        visible = true
        invulnerable = false
    }
}"""
    code = mdlite.render_code(sample, "pluma")
    return (
        hero
        + f'<section><h2 class="sec-h">Where to start</h2><div class="cards">{c}</div></section>'
        + '<section class="split"><div><h2 class="sec-h">Pluma reads like what it does</h2>'
        "<p>Static types, brace syntax, and a node tree you can edit live. Scripts hot-reload while the game runs, and a replay of the same inputs always reaches the same state hash.</p>"
        '<p><a href="@@SITE/pluma/guide.html">Read the Pluma guide</a></p></div>'
        f"<div>{code}</div></section>"
        + '<section><h2 class="sec-h">Build and run</h2>'
        + mdlite.render_code("cmake --preset debug && cmake --build --preset debug\nbuild/debug/runtime/cuervo --game examples/tutorial", "sh")
        + '<p>The <a href="@@SITE/manual/getting-started.html">getting started guide</a> has the details for your platform.</p></section>'
    )


# ---------------------------------------------------------------- output


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def finish(text, url, build_root="."):
    depth = url.count("/")
    prefix = "../" * depth
    return text.replace("@@SITE/", prefix).replace("@@ROOT", prefix or "./")


def search_entries(page):
    entries = []
    body = page.body
    parts = re.split(r'(<h[1-6] id="[^"]+">)', body)
    cur_id, cur_title = "", page.title
    pending = parts[0]

    def clean(h):
        t = re.sub(r"<[^>]+>", " ", h)
        t = html.unescape(t)
        t = re.sub(r"\s+", " ", t).replace(" #", "").strip()
        return t

    def flush(hid, heading, htmlchunk):
        text = clean(htmlchunk)
        if hid and text.startswith(heading):
            text = text[len(heading) :].strip()
        if not text and not heading:
            return
        entries.append([heading or page.title, page.url + (f"#{hid}" if hid else ""), page.title, text[:420]])

    heading = page.title
    hid = ""
    flush("", page.title, parts[0]) if clean(parts[0]) else None
    for k in range(1, len(parts), 2):
        m = re.match(r'<h[1-6] id="([^"]+)">', parts[k])
        hid = m.group(1)
        chunk = parts[k + 1]
        hm = re.match(r"(.*?)</h[1-6]>(.*)", chunk, re.S)
        heading = clean(hm.group(1)) if hm else ""
        rest = hm.group(2) if hm else chunk
        if page.api:
            rest = re.sub(r'<li id="m-[^"]+" class="api-item">.*?</li>', "", rest, flags=re.S)
        flush(hid, heading, "<p></p>" + rest)
    if page.api:
        for m in re.finditer(r'<li id="(m-[^"]+)" class="api-item">(.*?)</li>', body, re.S):
            text = clean(m.group(2))
            name = m.group(1)[2:]
            entries.append([f"{page.title}.{name}", f"{page.url}#{m.group(1)}", f"API: {page.title}", text[:300]])
    return entries


def check_site(out):
    """Every href/src that stays inside the site must exist, and so must its #fragment."""
    errors = []
    ids = {}

    def ids_of(path):
        if path not in ids:
            ids[path] = set(re.findall(r'\bid="([^"]+)"', path.read_text(encoding="utf-8", errors="replace"))) if path.suffix == ".html" else set()
        return ids[path]

    count = 0
    for f in sorted(out.rglob("*.html")):
        text = f.read_text(encoding="utf-8")
        for m in re.finditer(r'\b(?:href|src)="([^"]*)"', text):
            link = html.unescape(m.group(1))
            if not link or re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", link) or link.startswith("//"):
                continue
            count += 1
            path, _, frag = link.partition("#")
            target = f if not path else (f.parent / path)
            target = Path(os.path.normpath(target))
            if target.is_dir():
                target = target / "index.html"
            if not target.exists():
                errors.append(f"{f.relative_to(out)}: broken link {link!r}")
                continue
            if frag and target.suffix == ".html" and frag not in ids_of(target):
                errors.append(f"{f.relative_to(out)}: no anchor #{frag} in {target.relative_to(out)} (link {link!r})")
    return count, errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-o", "--out", required=True, help="output directory (replaced)")
    ap.add_argument("--font", action="append", default=[], metavar="ROLE=PATH", help="display|ui|ui-bold|mono TTF to ship (OFL)")
    ap.add_argument("--no-check", action="store_true", help="skip the internal link check")
    ap.add_argument("--allow-warnings", action="store_true", help="unresolvable Markdown links are not an error")
    ap.add_argument("--build-id", default="", help="text for the footer (default: the engine version)")
    args = ap.parse_args(argv)

    # The support page is generated from docs/support.json (milestone 8.8): the site must show
    # the same matrix the tier checks enforce, so a stale STATUS.md or README.md stops the build.
    if not args.no_check:
        import support_matrix  # noqa: E402

        stale = support_matrix.check()
        if stale:
            for m in stale:
                print(f"build_site: {m}", file=sys.stderr)
            return 1

    out = Path(args.out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    highlight.configure(ROOT / "tools/vscode-pluma/syntaxes/pluma.tmLanguage.json")
    pages, docs = build_pages(args)
    lay_out(pages, docs)
    targets = file_targets(pages)

    warnings = []
    for p in pages:
        for w in render_page(p, docs, targets) or []:
            warnings.append(f"{p.url}: {w}")
    add_api_extras(pages)

    pages_by_section = {}
    for p in pages:
        pages_by_section.setdefault(p.section, []).append(p)
    # The Pluma section lists the guide chapters under the guide.
    home = Page("index.html", "Home", "", body="")
    home.body = home_html(pages)
    home.description = "Cuervo Engine documentation: manual, Pluma language guide, tutorial and API reference."

    version = args.build_id
    if not version:
        m = re.search(r"project\(\s*\w+[^)]*VERSION\s+([\d.]+)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
        version = f"engine {m.group(1)}" if m else "engine"

    for p in pages + [home]:
        if p.section == "pluma" and p.url == "pluma/guide.html":
            pass
        write(out / p.url, finish(page_html(p, pages_by_section, version), p.url))

    # Assets: CSS (with the brand tokens), JS, brand marks, fonts and their licenses.
    tokens = json.loads((ROOT / "assets/brand/tokens.json").read_text(encoding="utf-8"))
    css = (HERE / "site/site.css").read_text(encoding="utf-8").replace("/*@TOKENS@*/", css_tokens(tokens))
    write(out / "assets/site.css", css)
    for name in ("site.js", "search.js"):
        shutil.copyfile(HERE / "site" / name, out / "assets" / name)
    # The wordmark is drawn in currentColor, which an <img> can't inherit: one copy per theme.
    wordmark = (ROOT / "assets/brand/logo/wordmark.svg").read_text(encoding="utf-8")
    ink = {"light": tokens["color"]["graphite"]["900"]["$value"], "dark": tokens["color"]["graphite"]["100"]["$value"]}
    for theme_name, color in ink.items():
        write(out / f"assets/brand/wordmark-{theme_name}.svg", wordmark.replace("currentColor", color))
    for name in ("mark-ember.svg", "mark-ember-dark.svg", "favicon.svg"):
        (out / "assets/brand").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "assets/brand/logo" / name, out / "assets/brand" / name)
    roles = {"display": "Outfit-Medium.ttf", "ui": "Figtree-Regular.ttf", "ui-bold": "Figtree-SemiBold.ttf", "mono": "JetBrainsMono-Regular.ttf"}
    fonts_given = dict(f.split("=", 1) for f in args.font)
    for role, fname in roles.items():
        src = fonts_given.get(role)
        if not src or not Path(src).is_file():
            print(f"build_site: missing --font {role}=<{fname}>", file=sys.stderr)
            return 2
        (out / "assets/fonts").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, out / "assets/fonts" / fname)
    for lic in ("Outfit", "Figtree", "JetBrainsMono"):
        shutil.copyfile(ROOT / "third_party/licenses" / f"{lic}.txt", out / "assets/fonts" / f"{lic}-OFL.txt")

    entries = []
    for p in pages:
        entries += search_entries(p)
    write(out / "assets/search-index.json", json.dumps(entries, separators=(",", ":"), ensure_ascii=False))

    print(f"build_site: {len(pages) + 1} pages, {len(entries)} search entries -> {out}")
    status = 0
    for w in warnings:
        print(f"build_site: warning: {w}", file=sys.stderr)
    if warnings and not args.allow_warnings:
        status = 1
    if not args.no_check:
        count, errors = check_site(out)
        print(f"build_site: checked {count} internal links")
        for e in errors:
            print(f"build_site: error: {e}", file=sys.stderr)
        if errors:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
