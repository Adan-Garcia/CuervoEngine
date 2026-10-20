"""Syntax highlighting for the docs site: Pluma, plus light-touch shell/TOML/JSON.

The Pluma keyword lists are read from the VS Code extension's TextMate grammar
(tools/vscode-pluma/syntaxes/pluma.tmLanguage.json) so the site, the editor
and the language agree on what a keyword is; `configure()` loads them. Without
it (unit tests) a built-in copy is used.
"""

import html
import json
import re

_FALLBACK = {
    "kw": "if else match for foreach in while loop break continue return step await yield start emit defer "
    "extends manual unsafe weak pub final static get set new alloc free is as",
    "decl": "var const fn class struct enum trait signal",
    "const": "true false null",
    "self": "self super",
    "ty": "int float bool str void i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 name List Map Task Signal Result",
}
WORDS = {k: set(v.split()) for k, v in _FALLBACK.items()}

_SCOPE_CLASS = {
    "keyword.control.pluma": "kw",
    "keyword.operator.memory.pluma": "kw",
    "keyword.operator.type.pluma": "kw",
    "storage.type.pluma": "decl",
    "storage.modifier.pluma": "kw",
    "constant.language.pluma": "const",
    "variable.language.pluma": "self",
    "support.type.pluma": "ty",
}


def configure(grammar_path):
    """Read keyword lists from the TextMate grammar; keeps the fallback when it can't."""
    try:
        with open(grammar_path, encoding="utf-8") as f:
            repo = json.load(f)["repository"]
    except (OSError, KeyError, ValueError):
        return False
    words = {k: set() for k in WORDS}
    for group in ("keywords", "types"):
        for p in repo.get(group, {}).get("patterns", []):
            cls = _SCOPE_CLASS.get(p.get("name", ""))
            m = re.search(r"\(\?:([\w|]+)\)", p.get("match", ""))
            if cls and m:
                words[cls].update(m.group(1).split("|"))
    words["ty"].update(["int", "float", "bool", "str", "void", "name", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"])
    if all(words[k] for k in ("kw", "decl", "const")):
        WORDS.update(words)
        return True
    return False


_PLUMA = re.compile(
    r"""
    (?P<cm>//[^\n]*|/\*.*?\*/)
  | (?P<str>r?\"\"\".*?\"\"\"|"(?:\\.|[^"\\\n])*")
  | (?P<at>@[A-Za-z_]\w*)
  | (?P<sym>\#[A-Za-z_]\w*)
  | (?P<num>\b(?:0x[0-9A-Fa-f_]+|0b[01_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?)(?:[iuf](?:8|16|32|64))?\b)
  | (?P<id>[A-Za-z_]\w*)
    """,
    re.X | re.S,
)
_VEC = re.compile(r"^i?vec\d$")


def _span(cls, text):
    return f'<span class="t-{cls}">{html.escape(text, quote=False)}</span>'


def highlight_pluma(code):
    out, pos, prev_word = [], 0, ""
    for m in _PLUMA.finditer(code):
        out.append(html.escape(code[pos : m.start()], quote=False))
        pos = m.end()
        kind, text = m.lastgroup, m.group()
        if kind != "id":
            out.append(_span({"cm": "cm", "str": "str", "at": "at", "sym": "sym", "num": "num"}[kind], text))
            prev_word = ""
            continue
        after = code[m.end() :].lstrip(" ")
        member = m.start() > 0 and code[m.start() - 1] == "."
        cls = None
        if not member:
            if text in WORDS["decl"]:
                cls = "decl"
            elif text in WORDS["kw"]:
                cls = "kw"
            elif text in WORDS["const"]:
                cls = "const"
            elif text in WORDS["self"]:
                cls = "self"
            elif text in WORDS["ty"] or _VEC.match(text):
                cls = "ty"
        if cls is None:
            if prev_word == "fn":
                cls = "fn"
            elif prev_word in ("class", "struct", "enum", "trait", "extends"):
                cls = "ty"
            elif after.startswith("("):
                cls = "call"
            elif not member and re.match(r"^[A-Z][A-Z0-9_]+$", text):
                cls = "cn"
            elif not member and re.match(r"^[A-Z]\w*$", text):
                cls = "ty"
        out.append(_span(cls, text) if cls else html.escape(text, quote=False))
        prev_word = text if not member else ""
    out.append(html.escape(code[pos:], quote=False))
    return "".join(out)


_SH = re.compile(r"""(?P<cm>(?:(?<=\s)|^)\#[^\n]*)|(?P<str>"(?:\\.|[^"\\\n])*"|'[^'\n]*')|(?P<kw>(?<=\s)--?[A-Za-z][\w-]*)""", re.M)
_TOML = re.compile(
    r"""(?P<cm>\#[^\n]*)|(?P<str>"(?:\\.|[^"\\\n])*"|'[^'\n]*')|(?P<ty>^\[[^\]\n]+\])|(?P<at>^[\w.-]+(?=\s*=))|(?P<const>\b(?:true|false)\b)|(?P<num>\b\d[\d_.]*\b)""",
    re.M,
)
_JSON = re.compile(r"""(?P<at>"(?:\\.|[^"\\\n])*"(?=\s*:))|(?P<str>"(?:\\.|[^"\\\n])*")|(?P<const>\b(?:true|false|null)\b)|(?P<num>-?\b\d[\d.eE+-]*\b)""")
_EBNF = re.compile(r"""(?P<cm>\(\*.*?\*\)|//[^\n]*)|(?P<str>"[^"\n]*"|'[^'\n]*')|(?P<ty>^\s*[A-Za-z_]\w*(?=\s*(?:=|::=)))""", re.S | re.M)


def _generic(code, rx):
    out, pos = [], 0
    for m in rx.finditer(code):
        out.append(html.escape(code[pos : m.start()], quote=False))
        out.append(_span(m.lastgroup, m.group()))
        pos = m.end()
    out.append(html.escape(code[pos:], quote=False))
    return "".join(out)


def highlight(code, lang):
    if lang == "pluma":
        return highlight_pluma(code)
    rx = {"sh": _SH, "toml": _TOML, "json": _JSON, "ebnf": _EBNF}.get(lang)
    return _generic(code, rx) if rx else html.escape(code, quote=False)
