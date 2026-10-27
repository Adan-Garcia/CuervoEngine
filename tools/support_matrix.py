#!/usr/bin/env python3
"""Generates the support matrix tables (milestone 8.8, DESIGN.md 4.4 and 4.16).

    python3 -I tools/support_matrix.py --write    # regenerate docs/STATUS.md and README.md
    python3 -I tools/support_matrix.py --check    # exit 1 when they are out of date (the ctest)

docs/support.json is the one source of truth: every feature with its status
(stable, experimental, planned, unsupported), the platforms it works on, the
desktop-only and web-compatible flags, the render tier it needs and a note.
The regions between `<!-- support-matrix:begin NAME -->` and
`<!-- support-matrix:end NAME -->` in docs/STATUS.md and README.md are
generated; the prose around them is hand-written. The C++ tier and export
checks compile the same file in (cuervo/script/support.cpp), and a doctest
compares its render-tier table with the RHI feature table in code, so the
documents and the code cannot drift apart. The documentation site is built
from those Markdown files (tools/build_site.py also runs --check).

Standard library only.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "docs" / "support.json"
STATUS = ROOT / "docs" / "STATUS.md"
README = ROOT / "README.md"

STATUSES = ["stable", "experimental", "planned", "unsupported"]
TIERS = ["none", "compatible", "full"]


def load(path=SOURCE):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def validate(data):
    """Returns a list of problems (empty when the data is consistent)."""
    errors = []
    targets = {t["id"]: t for t in data["targets"]}
    seen = set()
    for section in data["sections"]:
        for e in section["entries"]:
            where = f"{section['id']}/{e.get('id', '?')}"
            if e["id"] in seen:
                errors.append(f"{where}: duplicate id")
            seen.add(e["id"])
            if e["status"] not in STATUSES:
                errors.append(f"{where}: bad status {e['status']!r}")
            if e["tier"] not in TIERS:
                errors.append(f"{where}: bad tier {e['tier']!r}")
            for p in e["platforms"]:
                if p not in targets:
                    errors.append(f"{where}: unknown platform {p!r}")
            if e["web"] != ("web" in e["platforms"]):
                errors.append(f"{where}: 'web' flag disagrees with the platform list")
            if e["desktop_only"] != (bool(e["platforms"]) and "web" not in e["platforms"]):
                errors.append(f"{where}: 'desktop_only' flag disagrees with the platform list")
            if e["status"] in ("planned", "unsupported") and e["platforms"]:
                errors.append(f"{where}: a {e['status']} feature works on no platform")
            if e["status"] in ("stable", "experimental") and not e["platforms"]:
                errors.append(f"{where}: a {e['status']} feature needs at least one platform")
    # A target's own row (id == target id) must agree with the targets table.
    for section in data["sections"]:
        for e in section["entries"]:
            if e["id"] in targets and e["status"] != targets[e["id"]]["status"]:
                errors.append(f"target {e['id']}: status differs from its row in the platforms table")
    for name, tier in data["tiers"].items():
        for p in tier["platforms"]:
            if p not in targets:
                errors.append(f"tier {name}: unknown platform {p!r}")
    return errors


def title(status):
    return status.capitalize()


def where(data, e):
    if not e["platforms"]:
        return "-"
    names = {t["id"]: t for t in data["targets"]}
    parts = []
    for p in e["platforms"]:
        t = names[p]
        parts.append(t["name"] if t["status"] == "stable" else f"{t['name']} ({t['status']})")
    text = ", ".join(parts)
    return text + " (desktop only)" if e["desktop_only"] else text


def cell(text):
    return text.replace("|", "\\|")


def section_table(data, section):
    rows = ["| Feature | Level | Where | Tier | Notes |", "|---|---|---|---|---|"]
    for e in section["entries"]:
        tier = "-" if e["tier"] == "none" else f"`{e['tier']}`"
        rows.append(f"| {cell(e['name'])} | {title(e['status'])} | {where(data, e)} | {tier} | {cell(e['note'])} |")
    return "\n".join(rows)


def legend(data):
    rows = ["| Level | Meaning |", "|---|---|"]
    for lv in data["levels"]:
        rows.append(f"| **{lv['title']}** | {lv['meaning']} |")
    rows += [
        "",
        "Each row also says **where** the feature works and which render **tier** it needs. "
        "*Desktop only* means Linux and Windows but not the web; a row that lists Web is "
        "web-compatible. `-` in Tier means the feature does not depend on the render tier. "
        "`cuervo-tools tier check --platform <linux|windows|web>` and `cuervo-tools export` refuse a project "
        "that claims more than a row here provides for the target.",
    ]
    return "\n".join(rows)


def tier_text(data):
    out = []
    for name in ("compatible", "full"):
        t = data["tiers"][name]
        plat = {x["id"]: x["name"] for x in data["targets"]}
        out.append(f"### The `{name}` tier\n")
        out.append(t["summary"] + "\n")
        out.append(f"- **Backends:** {', '.join(t['backends'])}.")
        out.append(f"- **Platforms:** {', '.join(plat[p] for p in t['platforms'])}.")
        if t["limits"]:
            lim = t["limits"]
            out.append(
                f"- **Limits:** textures up to {lim['max_texture_size']} px, {lim['max_msaa_samples']}x MSAA, "
                f"{lim['max_uniform_block_size'] // 1024} KB per uniform range, {lim['max_vertex_attributes']} vertex "
                f"attributes, {lim['uniform_offset_alignment']}-byte uniform offset alignment (WebGL2 reality, "
                "not the GLES 3.0 spec minimums)."
            )
        for g in t["guarantees"]:
            out.append(f"- **Guarantee:** {g}")
        for r in t["refuses"]:
            out.append(f"- **Refused:** {r}")
        out.append("")
    rows = ["### RHI features by tier\n",
            "Generated from `docs/support.json`; the `support_tiers` test checks it against the feature table in "
            "`cuervo/rhi/rhi.cpp`.\n",
            "| RHI feature | Tier | Required | Notes |", "|---|---|---|---|"]
    for f in data["rhi_features"]:
        rows.append(f"| {f['name']} | `{f['tier']}` | {'yes' if f['required'] else 'optional'} | {cell(f['note'])} |")
    return "\n".join(out + rows)


def readme_summary(data):
    counts = {s: 0 for s in STATUSES}
    for section in data["sections"]:
        for e in section["entries"]:
            counts[e["status"]] += 1
    rows = ["| Platform | Level |", "|---|---|"]
    for e in next(s for s in data["sections"] if s["id"] == "platforms")["entries"]:
        rows.append(f"| {cell(e['name'])} | {title(e['status'])} |")
    line = ", ".join(f"{counts[s]} {s}" for s in STATUSES)
    rows += [
        "",
        f"{sum(counts.values())} features are classified ({line}). The `compatible` render tier runs "
        "everywhere; the `full` tier is desktop Vulkan only, and the tier check and the exporter refuse a "
        "project that claims more than its target provides. Details, per-feature platforms and the tier "
        "guarantees: [docs/STATUS.md](docs/STATUS.md).",
    ]
    return "\n".join(rows)


def regions(data):
    out = {"legend": legend(data), "tiers": tier_text(data)}
    for s in data["sections"]:
        out[s["id"]] = section_table(data, s)
    return out


def apply(text, name, body):
    pat = re.compile(
        r"(<!-- support-matrix:begin " + re.escape(name) + r" -->\n).*?(<!-- support-matrix:end " + re.escape(name) + r" -->)",
        re.S,
    )
    new, n = pat.subn(lambda m: m.group(1) + body + "\n" + m.group(2), text)
    if n != 1:
        raise SystemExit(f"support_matrix: region '{name}' not found exactly once")
    return new


def render_file(path, wanted):
    text = path.read_text(encoding="utf-8")
    for name, body in wanted.items():
        text = apply(text, name, body)
    return text


def targets_for(data):
    r = regions(data)
    status = {k: v for k, v in r.items()}
    return [(STATUS, status), (README, {"summary": readme_summary(data)})]


def check(data=None):
    """Returns a list of problems: data inconsistencies and stale documents."""
    data = data or load()
    problems = validate(data)
    for path, wanted in targets_for(data):
        if path.read_text(encoding="utf-8") != render_file(path, wanted):
            problems.append(f"{path.relative_to(ROOT)} is out of date: run python3 -I tools/support_matrix.py --write")
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)
    data = load()
    errors = validate(data)
    if errors:
        print("\n".join("support.json: " + e for e in errors), file=sys.stderr)
        return 1
    if args.write:
        for path, wanted in targets_for(data):
            path.write_text(render_file(path, wanted), encoding="utf-8")
        print("support matrix written to docs/STATUS.md and README.md")
        return 0
    problems = check(data)
    for p in problems:
        print(p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
