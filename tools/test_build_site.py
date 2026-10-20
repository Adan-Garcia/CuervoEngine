#!/usr/bin/env python3
"""Tests for the docs site generator (ctest `docs_site_unit`).

    python3 -I tools/test_build_site.py [--plm <path to plm>]

Covers the Markdown renderer and the highlighter on small inputs, and, with
--plm, runs every self-contained example of docs/PLUMA_GUIDE.md (the blocks that
define `fn main()`), so the guide's code cannot rot.
"""

import argparse
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "site"))

import highlight  # noqa: E402
import mdlite  # noqa: E402

PLM = None


def md(text, **kw):
    ctx = mdlite.Context(**kw)
    return mdlite.render(text.split("\n"), ctx), ctx


class MarkdownTests(unittest.TestCase):
    def test_paragraph_and_inline(self):
        out, _ = md("Hello `a<b>` and **bold** and *it* and snake_case_name.")
        self.assertIn("<p>Hello <code>a&lt;b&gt;</code> and <strong>bold</strong> and <em>it</em> and snake_case_name.</p>", out)

    def test_escapes_and_entities(self):
        out, _ = md("\\# not a heading, 1 &lt; 2 & 3 <tag>")
        self.assertIn("# not a heading", out)
        self.assertIn("&lt; 2 &amp; 3 &lt;tag&gt;", out)

    def test_heading_ids_follow_github(self):
        out, ctx = md("## 5. Tutorial: Starfall\n\n### `Canvas`\n\n## Twice\n\n## Twice")
        self.assertIn('id="5-tutorial-starfall"', out)
        self.assertIn('id="canvas"', out)
        self.assertEqual([h[2] for h in ctx.headings].count("twice"), 1)
        self.assertIn("twice-1", [h[2] for h in ctx.headings])

    def test_fence_is_highlighted_and_escaped(self):
        out, _ = md("```pluma\nvar x = 1 // <one>\n```")
        self.assertIn('<span class="t-decl">var</span>', out)
        self.assertIn("&lt;one&gt;", out)
        self.assertNotIn("<one>", out)

    def test_lists_nest_and_loose_items_get_paragraphs(self):
        out, _ = md("- a\n  - b\n- c\n\n1. x\n2. y")
        self.assertIn("<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul></li>\n<li>c</li>\n</ul>", out)
        self.assertIn("<ol>", out)
        loose, _ = md("- a\n\n  more\n\n- b")
        self.assertIn("<li><p>a</p>", loose)

    def test_table_with_escaped_pipe(self):
        out, _ = md("| A | B |\n|---|:-:|\n| `x\\|y` | 2 |")
        self.assertIn("<th>A</th>", out)
        self.assertIn('<td style="text-align:center">2</td>', out)
        self.assertIn("<code>x|y</code>", out)

    def test_links_are_resolved_through_the_context(self):
        out, ctx = md("[a](A.md#x) [b](nowhere.md) [c](https://example.com/z)", resolve=lambda h: "@@SITE/page.html" if h.startswith("A.md") else None)
        self.assertIn('<a href="@@SITE/page.html">a</a>', out)
        self.assertIn('class="nolink"', out)
        self.assertEqual(len(ctx.warnings), 1)
        self.assertIn('href="https://example.com/z"', out)

    def test_link_label_with_code(self):
        out, _ = md("[`Canvas`](#canvas)", resolve=lambda h: h)
        self.assertEqual(out, '<p><a href="#canvas"><code>Canvas</code></a></p>')

    def test_comments_quotes_rules(self):
        out, _ = md("<!-- hidden\n   still hidden -->\n\n> quoted\n> text\n\n---")
        self.assertNotIn("hidden", out)
        self.assertIn("<blockquote><p>quoted\ntext</p></blockquote>", out)
        self.assertIn("<hr>", out)

    def test_api_items_get_ids_and_highlighted_signatures(self):
        out, ctx = md("**Methods**\n\n- `fn circle(center: vec2) -> Color`\n\n  A disc.\n", api=True)
        self.assertIn('id="m-circle"', out)
        self.assertIn('<span class="t-fn">circle</span>', out)
        self.assertEqual(ctx.members, [("circle", "m-circle")])

    def test_level_shift_promotes_the_first_heading(self):
        out, ctx = md("### Step\n\n#### Inner", level_shift=2)
        self.assertIn("<h1 ", out)
        self.assertIn("<h2 ", out)


class HighlightTests(unittest.TestCase):
    def test_keywords_come_from_the_grammar(self):
        self.assertTrue(highlight.configure(HERE / "vscode-pluma/syntaxes/pluma.tmLanguage.json"))
        for word in ("foreach", "await", "defer", "extends"):
            self.assertIn(word, highlight.WORDS["kw"] | highlight.WORDS["decl"])

    def test_pluma_tokens(self):
        out = highlight.highlight_pluma('fn go(a: int) -> Foo { return #tag + 0xff + 1.5 // done\n}')
        for cls, text in (("decl", "fn"), ("fn", "go"), ("ty", "int"), ("ty", "Foo"), ("kw", "return"), ("sym", "#tag"), ("num", "0xff"), ("num", "1.5"), ("cm", "// done")):
            self.assertIn(f'<span class="t-{cls}">{text}</span>', out)

    def test_members_are_not_keywords(self):
        out = highlight.highlight_pluma("map.get(k)")
        self.assertNotIn("t-kw", out)
        self.assertIn('<span class="t-call">get</span>', out)

    def test_other_languages_do_not_crash_and_escape(self):
        for lang in ("sh", "toml", "json", "ebnf", ""):
            self.assertIn("&lt;", highlight.highlight("a <b> # c\n\"x\" 1", lang))


class GuideExampleTests(unittest.TestCase):
    @unittest.skipUnless(lambda: PLM, "needs --plm")
    def test_guide_programs_run(self):
        if not PLM:
            self.skipTest("no --plm given")
        text = (HERE.parent / "docs/PLUMA_GUIDE.md").read_text(encoding="utf-8")
        blocks = [b for b in re.findall(r"^```pluma\n(.*?)^```", text, re.S | re.M) if "fn main()" in b]
        self.assertGreaterEqual(len(blocks), 8)
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            for i, b in enumerate(blocks):
                path = Path(tmp) / f"example{i}.plm"
                path.write_text(b, encoding="utf-8")
                r = subprocess.run([PLM, str(path)], capture_output=True, text=True, env=dict(os.environ))
                self.assertEqual(r.returncode, 0, f"PLUMA_GUIDE.md example {i} failed:\n{b}\n{r.stdout}{r.stderr}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--plm")
    ns, rest = ap.parse_known_args()
    PLM = ns.plm
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=1)
