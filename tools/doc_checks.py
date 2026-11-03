"""Consistency checks between the documents and their sources of truth (milestone 11.5).

`docs_site` runs `check_repo()` and fails when a document states

- a version (`Beta (v0.2)`) whose major.minor differs from the CMake project version,
- the support level of a platform (`Windows is experimental`) that differs from
  docs/support.json,
- a milestone or phase number that is not a row of the DESIGN.md section 6 table,
- the width of `int` (`int` is 64-bit) that differs from the language reference.

Every check works on plain text so the unit tests can feed it inconsistent fixtures.
Standard library only.
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# DESIGN.md is the source of the milestone table and ROADMAP.md is generated from it.
SKIP_CLAIMS = {"docs/DESIGN.md", "docs/ROADMAP.md"}

PLATFORM_WORDS = {"linux": "linux", "windows": "windows", "web": "web", "the web": "web"}


def cmake_version(cmake_text):
    m = re.search(r"project\(\s*\w+[^)]*?VERSION\s+(\d+)\.(\d+)\.(\d+)", cmake_text)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def milestone_ids(design_text):
    """Row ids of the DESIGN.md section 6 table ('5.5', '10.5a', '11')."""
    start = design_text.find("## 6. Milestones")
    if start < 0:
        return set()
    return set(re.findall(r"^\| (\d+(?:\.\d+)?[a-z]?) \|", design_text[start:], re.M))


def language_int_bits(language_text):
    m = re.search(r"`int` is (\d+)-bit", language_text)
    return int(m.group(1)) if m else None


def platform_levels(support):
    """{'linux': 'stable', ...} from the parsed docs/support.json."""
    return {t["id"]: t["status"] for t in support["targets"]}


def _line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def check_versions(name, text, version):
    """`Beta (vX.Y)` and `version X.Y.Z` style claims must match the CMake version."""
    errors = []
    if version is None:
        return errors
    major_minor = f"{version[0]}.{version[1]}"
    full = f"{version[0]}.{version[1]}.{version[2]}"
    for m in re.finditer(r"\b(?:Beta|Alpha|Release|Version|version)\s*\(?v?(\d+\.\d+)(\.\d+)?\)?", text):
        said = m.group(1) + (m.group(2) or "")
        ok = said == full if m.group(2) else said == major_minor
        if not ok:
            errors.append(f"{name}:{_line_of(text, m.start())}: states version {said}, the CMake project version is {full}")
    return errors


def check_platforms(name, text, levels):
    """`Windows is experimental` style claims must match docs/support.json targets."""
    errors = []
    pattern = re.compile(
        r"\b(Linux|Windows|the web|Web)\b(?:\s*\([^)]*\))?\s+(?:is|are|remains?|stays?)\s+(?:still\s+)?(?:an?\s+)?(stable|experimental|planned|unsupported)\b",
        re.I,
    )
    for m in pattern.finditer(text):
        key = PLATFORM_WORDS.get(m.group(1).lower())
        said = m.group(2).lower()
        truth = levels.get(key)
        if truth is not None and said != truth:
            errors.append(f"{name}:{_line_of(text, m.start())}: calls {m.group(1)} {said}, docs/support.json says {truth}")
    return errors


def check_milestones(name, text, ids):
    """`Milestone 10.5a` and `Phase 8` must name rows of the DESIGN.md section 6 table."""
    errors = []
    if not ids:
        return errors
    for m in re.finditer(r"\b(Milestone|milestone|Phase|phase)s? (\d+(?:\.\d+)?[a-z]?)\b", text):
        if m.group(2) not in ids:
            errors.append(f"{name}:{_line_of(text, m.start())}: names {m.group(1).lower()} {m.group(2)}, which is not a row of the DESIGN.md section 6 table")
    return errors


def check_int_width(name, text, bits):
    """`int` is N-bit claims must match the language reference."""
    errors = []
    if bits is None:
        return errors
    for m in re.finditer(r"`int`\s*(?:is|are|=)?\s*(?:a\s+)?(\d+)-bit", text):
        if int(m.group(1)) != bits:
            errors.append(f"{name}:{_line_of(text, m.start())}: says `int` is {m.group(1)}-bit, LANGUAGE.md says {bits}")
    return errors


def check_texts(docs, version, levels, ids, bits):
    """docs: {repo-relative path: text}. Returns a list of error strings."""
    errors = []
    for name, text in sorted(docs.items()):
        if name in SKIP_CLAIMS:
            continue
        # Fenced code is examples, not claims about the project.
        prose = re.sub(r"^```.*?^```", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S | re.M)
        errors += check_versions(name, prose, version)
        errors += check_platforms(name, prose, levels)
        errors += check_milestones(name, prose, ids)
        errors += check_int_width(name, prose, bits)
    return errors


def design_header_errors(design_text, version):
    """The DESIGN.md status line is a claim too, even though the file is the milestone source."""
    return check_versions("docs/DESIGN.md", "\n".join(design_text.split("\n")[:12]), version)


GIF_LIMIT = 1_500_000
FORBIDDEN_IN_GIF = (b"agent", b"anthro" b"pic", b"assistant", b"generated by")


def check_landing(readme_text, root):
    """The README (and the site's landing page, which is built from the same sections) opens with a
    gameplay GIF, a quickstart and the benchmark table; the GIF is small and carries no metadata."""
    errors = []
    root = Path(root)
    gifs = re.findall(r'src="(docs/assets/[^"]+\.gif)"', readme_text)
    if not gifs:
        errors.append("README.md: no gameplay GIF (<img src=\"docs/assets/....gif\">)")
    for g in gifs:
        f = root / g
        if not f.is_file():
            errors.append(f"README.md: {g} does not exist")
            continue
        data = f.read_bytes()
        if not data.startswith(b"GIF89a"):
            errors.append(f"{g}: not a GIF")
        if len(data) >= GIF_LIMIT:
            errors.append(f"{g}: {len(data)} bytes, the limit is {GIF_LIMIT}")
        if b"\x21\xfe" in data[:800] or any(w in data.lower() for w in FORBIDDEN_IN_GIF):
            errors.append(f"{g}: has a comment block or a tool name in its metadata")
    heads = [m.group(1) for m in re.finditer(r"^## (.+)$", readme_text, re.M)]
    for need in ("Quickstart", "How fast"):
        if need not in heads:
            errors.append(f"README.md: no '## {need}' section")
    if "Quickstart" in heads and "Documentation" in heads and heads.index("Quickstart") > heads.index("Documentation"):
        errors.append("README.md: '## Quickstart' must come before '## Documentation'")
    if gifs and "Quickstart" in heads and readme_text.find(gifs[0]) > readme_text.find("## Quickstart"):
        errors.append("README.md: the gameplay GIF must open the page, before the quickstart")
    m = re.search(r"^## How fast\n(.*?)(?=^## )", readme_text, re.M | re.S)
    if m and not re.search(r"^\|.*\|\n\|[-| ]+\|", m.group(1), re.M):
        errors.append("README.md: '## How fast' has no benchmark table")
    return errors


def check_repo(root=ROOT):
    root = Path(root)

    def read(p):
        return (root / p).read_text(encoding="utf-8")

    version = cmake_version(read("CMakeLists.txt"))
    design = read("docs/DESIGN.md")
    levels = platform_levels(json.loads(read("docs/support.json")))
    ids = milestone_ids(design)
    bits = language_int_bits(read("docs/LANGUAGE.md"))
    names = ["README.md", "CONTRIBUTING.md", "editor/README.md"] + sorted(str(p.relative_to(root)).replace("\\", "/") for p in (root / "docs").glob("*.md"))
    docs = {n: read(n) for n in names if (root / n).is_file()}
    errors = check_texts(docs, version, levels, ids, bits)
    errors += design_header_errors(design, version)
    errors += check_landing(read("README.md"), root)
    if bits is None:
        errors.append("docs/LANGUAGE.md: no `int` is N-bit statement to check against")
    return errors
