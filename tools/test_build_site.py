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
sys.path.insert(0, str(HERE))

import doc_checks  # noqa: E402
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


class ConsistencyTests(unittest.TestCase):
    """Deliberately inconsistent fixtures (milestone 11.5): each check must fail on them."""

    VERSION = (0, 2, 2)
    LEVELS = {"linux": "stable", "windows": "stable", "web": "stable"}
    IDS = {"8", "8.1", "10", "10.5a", "11", "11.5"}

    def run_checks(self, text, name="docs/X.md"):
        return doc_checks.check_texts({name: text}, self.VERSION, self.LEVELS, self.IDS, 32)

    def test_repo_is_consistent(self):
        self.assertEqual(doc_checks.check_repo(HERE.parent), [])

    def test_clean_fixture_passes(self):
        self.assertEqual(self.run_checks("Beta (v0.2). Windows is stable. See milestone 10.5a. `int` is 32-bit."), [])

    def test_wrong_version(self):
        errs = self.run_checks("Status: Beta (v0.1)")
        self.assertEqual(len(errs), 1)
        self.assertIn("0.1", errs[0])
        self.assertTrue(self.run_checks("version 0.2.1"))
        self.assertEqual(self.run_checks("version 0.2.2"), [])

    def test_wrong_platform_level(self):
        errs = self.run_checks("Linux and the web are supported; Windows is experimental and still being validated.")
        self.assertEqual(len(errs), 1)
        self.assertIn("support.json says stable", errs[0])
        self.assertTrue(self.run_checks("The web is unsupported."))
        # Another platform or a feature, not in support.json targets: not a platform claim.
        self.assertEqual(self.run_checks("macOS is unsupported. Web saves are experimental."), [])

    def test_unknown_milestone_and_phase(self):
        self.assertTrue(self.run_checks("Milestone 99.1 added it."))
        self.assertTrue(self.run_checks("see Phase 7 for details"))
        self.assertEqual(self.run_checks("Milestone 11.5 and phase 8"), [])

    def test_int_width(self):
        errs = self.run_checks("Numbers. `int` is 64-bit, `float` is 32-bit.")
        self.assertEqual(len(errs), 1)
        self.assertIn("LANGUAGE.md says 32", errs[0])

    def test_code_fences_are_not_claims(self):
        self.assertEqual(self.run_checks("```\nWindows is experimental; `int` is 64-bit; milestone 99\n```\n"), [])

    def test_generated_and_source_docs_are_skipped(self):
        self.assertEqual(self.run_checks("Milestone 99", name="docs/ROADMAP.md"), [])

    def test_design_header_version(self):
        self.assertTrue(doc_checks.design_header_errors("# T\n\n> Status: Beta (v0.1).", self.VERSION))
        self.assertEqual(doc_checks.design_header_errors("# T\n\n> Status: Beta (v0.2).", self.VERSION), [])

    def test_landing_requires_gif_quickstart_and_table(self):
        import tempfile

        good = '<img src="docs/assets/a.gif">\n\n## Quickstart\n\nrun\n\n## How fast\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n## Documentation\n'
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "docs/assets").mkdir(parents=True)
            gif = Path(d) / "docs/assets/a.gif"
            gif.write_bytes(b"GIF89a" + b"\0" * 100)
            self.assertEqual(doc_checks.check_landing(good, d), [])
            self.assertTrue(doc_checks.check_landing(good.replace("## Quickstart", "## Intro"), d))
            self.assertTrue(doc_checks.check_landing(good.replace("| a | b |\n|---|---|\n| 1 | 2 |", "no table"), d))
            self.assertTrue(doc_checks.check_landing(good.replace("a.gif", "missing.gif"), d))
            self.assertTrue(doc_checks.check_landing("## Quickstart\n## How fast\n| a |\n|---|\n", d))  # no GIF
            gif.write_bytes(b"GIF89a" + b"\0" * 2_000_000)
            self.assertTrue(doc_checks.check_landing(good, d))
            gif.write_bytes(b"GIF89a" + b"made by Agent")
            self.assertTrue(doc_checks.check_landing(good, d))

    def test_sources_of_truth_parse(self):
        self.assertEqual(doc_checks.cmake_version("project(cuervo VERSION 1.4.7 LANGUAGES C CXX)"), (1, 4, 7))
        self.assertEqual(doc_checks.language_int_bits("(`int` is 32-bit)"), 32)
        self.assertEqual(doc_checks.milestone_ids("## 6. Milestones\n| 5.5 | x |\n| 10.5a | y |\n| 11 | z |"), {"5.5", "10.5a", "11"})


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
