"""A small Markdown renderer for the docs site (tools/build_site.py).

Python's standard library has no Markdown, and the site may not add pip
dependencies (DESIGN.md 4.16 "Clarified (Milestone 8.4)"), so this implements
the subset the repo's documents use: ATX headings, paragraphs, fenced code
(highlighted by `highlight.py`), bullet and numbered lists with nested blocks,
tables, block quotes, rules, HTML comments, raw HTML blocks, and the inline
code / emphasis / link / autolink / escape forms.

It is deterministic and has no state besides what `Context` carries.
"""

import html
import re

from highlight import highlight, highlight_pluma

FENCE_RE = re.compile(r"^(\s*)(`{3,}|~{3,})\s*([\w+-]*)\s*$")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)(?:\s+#+)?\s*$")
LIST_RE = re.compile(r"^(\s*)([-*+]|\d{1,3}[.)])(\s+)(\S.*)?$")
HR_RE = re.compile(r"^\s*([-*_])(?:\s*\1){2,}\s*$")
TABLE_SEP_RE = re.compile(r"^\s*\|?\s*:?-{1,}:?\s*(?:\|\s*:?-{1,}:?\s*)*\|?\s*$")
RAW_HTML_RE = re.compile(r"^\s*<(p|div|picture|img|details|summary|table|center|br|source)\b", re.I)
SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


def strip_markup(text):
    """Plain text of an inline Markdown string (for ids, titles, search)."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"`+([^`]*)`+", r"\1", text)
    text = re.sub(r"\\([\\`*_{}\[\]()#+\-.!|<>])", r"\1", text)
    text = text.replace("**", "").replace("__", "")
    text = re.sub(r"(?<!\w)[*_](\S(?:.*?\S)?)[*_](?!\w)", r"\1", text)
    return html.unescape(text).strip()


def slugify(text):
    """GitHub's heading anchor: lowercase, punctuation dropped, spaces to hyphens."""
    t = strip_markup(text).lower()
    t = re.sub(r"[^\w\- ]", "", t)
    return t.replace(" ", "-")


class Context:
    """Per-render state: link resolution, heading ids, collected headings."""

    def __init__(self, resolve=None, slug_at=None, base_line=0, level_shift=0, api=False, h1_text=None):
        self.resolve = resolve or (lambda href: href)  # href -> url, or None when it points nowhere
        self.slug_at = slug_at or {}  # absolute line index -> heading id
        self.base_line = base_line
        self.level_shift = level_shift  # subtracted from every heading level
        self.api = api  # list items starting with a signature get ids
        self.h1_text = h1_text  # replaces the text of the first (promoted) heading
        self.headings = []  # (level, text, id)
        self.members = []  # (name, id) of API items
        self.used_ids = set()
        self.warnings = []
        self._first_heading = True

    def fresh_id(self, text):
        base = slugify(text) or "section"
        slug, n = base, 0
        while slug in self.used_ids:
            n += 1
            slug = f"{base}-{n}"
        self.used_ids.add(slug)
        return slug


# ---------------------------------------------------------------- inline

_CODE_RE = re.compile(r"(`+)(.+?)(?<!`)\1(?!`)", re.S)
_ESC_RE = re.compile(r"\\([\\`*_{}\[\]()#+\-.!|<>])")
_AUTOLINK_RE = re.compile(r"<((?:https?|mailto):[^>\s]+)>")
_LINK_RE = re.compile(r"\[((?:[^\[\]]|\[[^\]]*\])*)\]\(\s*<?([^)\s>]*)>?(?:\s+\"([^\"]*)\")?\s*\)")
_BOLD_RE = re.compile(r"(\*\*|__)(?=\S)(.+?)(?<=\S)\1", re.S)
_ITAL_STAR_RE = re.compile(r"(?<![\w*])\*(?=[^\s*])(.+?)(?<=[^\s*])\*(?![\w*])", re.S)
_ITAL_UND_RE = re.compile(r"(?<![\w])_(?=[^\s_])(.+?)(?<=[^\s_])_(?![\w])", re.S)
_URL_RE = re.compile(r"(?<![\w\"'=/])(https?://[^\s<>()\"']+[^\s<>()\"'.,;:!?])")
_ENTITY_AMP_RE = re.compile(r"&(?!#?\w+;)")


_uid = [0]


def render_inline(text, ctx):
    stash = []
    _uid[0] += 1
    uid = _uid[0]  # tokens are per call: a link label is rendered by a nested call

    def put(fragment):
        stash.append(fragment)
        return f"\x00{uid}:{len(stash) - 1}\x00"

    def code_sub(m):
        body = m.group(2)
        if body.startswith(" ") and body.endswith(" ") and body.strip():
            body = body[1:-1]
        return put(f"<code>{html.escape(body, quote=False)}</code>")

    text = _CODE_RE.sub(code_sub, text)
    text = _ESC_RE.sub(lambda m: put(html.escape(m.group(1), quote=False)), text)
    text = _AUTOLINK_RE.sub(lambda m: put(f'<a href="{html.escape(m.group(1))}" rel="noopener">{html.escape(m.group(1))}</a>'), text)

    def link_sub(m):
        label, href, title = m.group(1), m.group(2), m.group(3)
        inner = render_inline(label, ctx)
        if SCHEME_RE.match(href):
            url, extra = href, ' rel="noopener"'
        else:
            url, extra = ctx.resolve(href), ""
        t = f' title="{html.escape(title)}"' if title else ""
        if url is None:
            ctx.warnings.append(f"link to {href!r} does not point at a page of the site")
            return put(f'<span class="nolink" title="in the repository: {html.escape(href)}">{inner}</span>')
        return put(f'<a href="{html.escape(url)}"{t}{extra}>{inner}</a>')

    text = _LINK_RE.sub(link_sub, text)
    text = _URL_RE.sub(lambda m: put(f'<a href="{html.escape(m.group(1))}" rel="noopener">{html.escape(m.group(1))}</a>'), text)
    text = _ENTITY_AMP_RE.sub("&amp;", text)
    text = text.replace("<", "&lt;").replace(">", "&gt;")
    text = _BOLD_RE.sub(lambda m: f"<strong>{m.group(2)}</strong>", text)
    text = _ITAL_STAR_RE.sub(lambda m: f"<em>{m.group(1)}</em>", text)
    text = _ITAL_UND_RE.sub(lambda m: f"<em>{m.group(1)}</em>", text)
    text = re.sub(r"(?: {2,}|\\)\n", "<br>\n", text)
    # Placeholders may nest (a link whose label holds code).
    token = re.compile(rf"\x00{uid}:(\d+)\x00")
    for _ in range(4):
        text = token.sub(lambda m: stash[int(m.group(1))], text)
    return text


# ---------------------------------------------------------------- blocks


def _indent(line):
    return len(line) - len(line.lstrip(" "))


def _starts_block(line, in_paragraph=True):
    if not line.strip():
        return True
    if FENCE_RE.match(line) or HEADING_RE.match(line) or HR_RE.match(line):
        return True
    if line.lstrip().startswith(">") or line.lstrip().startswith("<!--"):
        return True
    m = LIST_RE.match(line)
    if m and m.group(4) is not None:
        if not in_paragraph:
            return True
        marker = m.group(2)
        return not marker[0].isdigit() or marker.startswith("1")
    return False


def _split_row(line):
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]
    cells, cur = [], ""
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] == "|":
            cur += "|"
            i += 2
            continue
        if s[i] == "|":
            cells.append(cur.strip())
            cur = ""
        else:
            cur += s[i]
        i += 1
    cells.append(cur.strip())
    return cells


def _api_member(first_line):
    """(name, id) for an API item whose text starts with a `signature`."""
    m = re.match(r"^`(.+?)`\s*$", first_line.strip())
    if not m:
        return None
    sig = m.group(1)
    sig = re.sub(r"@\w+(\([^)]*\))?\s*", "", sig)
    m2 = re.search(r"\b(?:fn|var|signal|const)\s+(\w+)", sig)
    name = m2.group(1) if m2 else (re.match(r"(\w+)", sig) or [None, None])[1]
    return (name, "m-" + name) if name else None


def render_blocks(lines, ctx, tight=False, top=True):
    out = []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue

        m = FENCE_RE.match(line)
        if m:
            ind, fence, lang = len(m.group(1)), m.group(2), m.group(3)
            body = []
            i += 1
            while i < n:
                if lines[i].strip().startswith(fence[0] * len(fence)) and not lines[i].strip().strip(fence[0]):
                    break
                body.append(lines[i][ind:] if lines[i][:ind].strip() == "" else lines[i])
                i += 1
            i += 1
            out.append(render_code("\n".join(body), lang))
            continue

        if line.lstrip().startswith("<!--"):
            while i < n and "-->" not in lines[i]:
                i += 1
            i += 1
            continue

        m = HEADING_RE.match(line)
        if m:
            level, text = len(m.group(1)), m.group(2)
            lvl = max(1, level - ctx.level_shift) if top else level
            hid = ctx.slug_at.get(ctx.base_line + i) if top else None
            if hid is None:
                hid = ctx.fresh_id(text)
            else:
                ctx.used_ids.add(hid)
            show = text
            if ctx._first_heading and ctx.h1_text is not None:
                show = ctx.h1_text
            ctx._first_heading = False
            ctx.headings.append((lvl, strip_markup(show), hid))
            out.append(f'<h{lvl} id="{hid}">{render_inline(show, ctx)}<a class="anchor" href="#{hid}" aria-label="Link to this section">#</a></h{lvl}>')
            i += 1
            continue

        if HR_RE.match(line):
            out.append("<hr>")
            i += 1
            continue

        if line.lstrip().startswith(">"):
            quote = []
            while i < n and (lines[i].lstrip().startswith(">") or (lines[i].strip() and quote and not _starts_block(lines[i]))):
                quote.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            out.append("<blockquote>" + render_blocks(quote, ctx, top=False) + "</blockquote>")
            continue

        if RAW_HTML_RE.match(line):
            raw = []
            while i < n and lines[i].strip():
                raw.append(lines[i])
                i += 1
            out.append("\n".join(raw))
            continue

        if "|" in line and i + 1 < n and TABLE_SEP_RE.match(lines[i + 1]) and "|" in line and "|" in lines[i + 1]:
            head = _split_row(line)
            aligns = []
            for c in _split_row(lines[i + 1]):
                aligns.append("center" if c.startswith(":") and c.endswith(":") else "right" if c.endswith(":") else "")
            i += 2
            rows = []
            while i < n and lines[i].strip() and "|" in lines[i]:
                rows.append(_split_row(lines[i]))
                i += 1

            def cell(tag, text, k):
                a = f' style="text-align:{aligns[k]}"' if k < len(aligns) and aligns[k] else ""
                return f"<{tag}{a}>{render_inline(text, ctx)}</{tag}>"

            t = ['<div class="table-wrap"><table><thead><tr>' + "".join(cell("th", c, k) for k, c in enumerate(head)) + "</tr></thead><tbody>"]
            for r in rows:
                t.append("<tr>" + "".join(cell("td", c, k) for k, c in enumerate(r)) + "</tr>")
            t.append("</tbody></table></div>")
            out.append("".join(t))
            continue

        m = LIST_RE.match(line)
        if m and m.group(4) is not None:
            html_list, i = render_list(lines, i, ctx)
            out.append(html_list)
            continue

        para = [line.strip()]
        i += 1
        while i < n and not _starts_block(lines[i]):
            if "|" in lines[i] and i + 1 < n and TABLE_SEP_RE.match(lines[i + 1]) and "|" in lines[i + 1]:
                break
            para.append(lines[i].rstrip() if lines[i].endswith("  ") else lines[i].strip())
            i += 1
        text = "\n".join(para)
        out.append(render_inline(text, ctx) if tight else f"<p>{render_inline(text, ctx)}</p>")
    sep = "\n" if tight else "\n"
    return sep.join(out)


def render_list(lines, i, ctx):
    n = len(lines)
    m = LIST_RE.match(lines[i])
    base = len(m.group(1))
    ordered = m.group(2)[0].isdigit()
    start = int(m.group(2)[:-1]) if ordered else 1
    items, loose = [], False
    while i < n:
        m = LIST_RE.match(lines[i])
        if not m or m.group(4) is None or abs(len(m.group(1)) - base) > 1 or m.group(2)[0].isdigit() != ordered:
            break
        spaces = len(m.group(3)) if len(m.group(3)) <= 4 else 1
        content = len(m.group(1)) + len(m.group(2)) + spaces
        body = [m.group(4)]
        i += 1
        while i < n:
            ln = lines[i]
            if not ln.strip():
                j = i
                while j < n and not lines[j].strip():
                    j += 1
                if j < n and _indent(lines[j]) >= content:
                    body.extend([""] * (j - i))
                    i = j
                    continue
                nm = LIST_RE.match(lines[j]) if j < n else None
                if nm and abs(_indent(lines[j]) - base) <= 1 and nm.group(2)[0].isdigit() == ordered:
                    loose = True
                i = j
                break
            if _indent(ln) >= content:
                body.append(ln[content:])
                i += 1
                continue
            lm = LIST_RE.match(ln)
            if lm and lm.group(4) is not None:
                break
            if _starts_block(ln) or not body[-1].strip():
                break
            body.append(ln.strip())  # lazy continuation
            i += 1
        items.append(body)
        if i < n and not lines[i].strip():
            break
    for body in items:
        for k in range(1, len(body)):
            if body[k - 1] == "" and body[k] and not body[k].startswith(" ") and not LIST_RE.match(body[k]) and k > 1:
                loose = True
    tag = "ol" if ordered else "ul"
    attrs = f' start="{start}"' if ordered and start != 1 else ""
    parts = [f"<{tag}{attrs}>"]
    for body in items:
        attr = ""
        if ctx.api:
            mem = _api_member(body[0])
            if mem:
                attr = f' id="{mem[1]}" class="api-item"'
                ctx.members.append(mem)
        if attr:
            # An API item: its signature is Pluma, so highlight it like a code block.
            sig = re.match(r"^`(.+?)`\s*$", body[0].strip()).group(1)
            inner = f'<p class="sig"><code>{highlight_pluma(sig)}</code></p>\n' + render_blocks(body[1:], ctx, tight=False, top=False)
        else:
            inner = render_blocks(body, ctx, tight=not loose, top=False)
        parts.append(f"<li{attr}>{inner}</li>")
    parts.append(f"</{tag}>")
    return "\n".join(parts), i


def render_code(code, lang):
    label = {"pluma": "Pluma", "sh": "Shell", "toml": "TOML", "json": "JSON", "ebnf": "EBNF"}.get(lang, "")
    cls = f"lang-{lang}" if lang else "lang-text"
    head = f'<span class="code-lang">{label}</span>' if label else ""
    return (
        f'<div class="codeblock {cls}">{head}<button class="copy" type="button" aria-label="Copy code">Copy</button>'
        f"<pre><code>{highlight(code, lang)}</code></pre></div>"
    )


def render(markdown_lines, ctx):
    return render_blocks(markdown_lines, ctx)
