#!/usr/bin/env python3
"""Copies the Cuervo brand assets from the design kit into assets/brand/.

The design kit (DESIGN.md 4.16, milestone 8.1) is its own git repository outside
this one. Exports and the web build need the files in-tree, so this script copies
the logo SVGs, the PNG exports and tokens.json into assets/brand/, records the
kit's source commit in assets/brand/SOURCE.txt and writes assets/brand/README.md.
It is the only way those files change: never edit them by hand, edit the kit and
run this again. The kit itself is only read.

    python3 -I tools/sync_brand.py [--kit <design kit dir>] [--check]

--kit defaults to $CUERVO_DESIGN_KIT, else /mnt/storage/CuervoSpine/design.
--check compares instead of writing and exits 1 when assets/brand/ is out of
date with the kit (or was edited by hand). --verify needs no kit: it checks the
files against the hashes in SOURCE.txt (the `brand_assets` ctest). Standard
library only.
"""

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEST = REPO / "assets" / "brand"
DEFAULT_KIT = "/mnt/storage/CuervoSpine/design"

# Kit path -> path under assets/brand/. Only the visual identity: the mark,
# the wordmark and lockups, the icons, and the generated design tokens
# (tokens.css is the kit's source of truth; tokens.json is generated from it
# by the kit's build_tokens.py and is what the engine reads).
FILES = {
    "logo/mark.svg": "logo/mark.svg",
    "logo/mark-ember.svg": "logo/mark-ember.svg",
    "logo/mark-ember-dark.svg": "logo/mark-ember-dark.svg",
    "logo/mark-raven.svg": "logo/mark-raven.svg",
    "logo/mark-bone.svg": "logo/mark-bone.svg",
    "logo/mark-small.svg": "logo/mark-small.svg",
    "logo/mark-small-ember.svg": "logo/mark-small-ember.svg",
    "logo/wordmark.svg": "logo/wordmark.svg",
    "logo/lockup-horizontal.svg": "logo/lockup-horizontal.svg",
    "logo/lockup-stacked.svg": "logo/lockup-stacked.svg",
    "logo/app-icon.svg": "logo/app-icon.svg",
    "logo/favicon.svg": "logo/favicon.svg",
    "logo/png/app-icon-512.png": "png/app-icon-512.png",
    "logo/png/favicon-16.png": "png/favicon-16.png",
    "logo/png/favicon-32.png": "png/favicon-32.png",
    "logo/png/favicon-48.png": "png/favicon-48.png",
    "logo/png/mark-ember-256.png": "png/mark-ember-256.png",
    "logo/png/mark-ember-512.png": "png/mark-ember-512.png",
    "logo/png/mark-ember-dark-256.png": "png/mark-ember-dark-256.png",
    "logo/png/mark-ember-dark-512.png": "png/mark-ember-dark-512.png",
    "logo/png/mark-raven-256.png": "png/mark-raven-256.png",
    "tokens/tokens.json": "tokens.json",
}

README = """\
# Brand assets (generated: do not edit)

The Cuervo Mark, wordmark, lockups, icons and design tokens, copied from the Cuervo
design kit by `tools/sync_brand.py` (DESIGN.md 4.16, milestone 8.1). The kit is the
source; `SOURCE.txt` records the kit commit these files came from and each file's
SHA-256. **That script is the only way the files here change**: edit the kit, then

```sh
python3 -I tools/sync_brand.py            # copy from the kit (default /mnt/storage/CuervoSpine/design)
python3 -I tools/sync_brand.py --check    # exit 1 if this folder differs from the kit
python3 -I tools/sync_brand.py --verify   # no kit needed: files match SOURCE.txt (ctest brand_assets)
```

and commit the result. Only the visual identity applies to the engine (mark, wordmark,
palette, type); the kit's product copy and flows are for another product.

| Path | Use in the engine |
|---|---|
| `tokens.json` | W3C design tokens. CMake generates `cuervo/brand/brand_tokens.hpp` (light and dark themes) from it at configure time (`cmake/Brand.cmake`) |
| `png/app-icon-512.png` | Embedded in the engine: window icon of `cuervo` and `cuervo-editor` |
| `logo/mark-ember.svg`, `logo/mark-ember-dark.svg`, `logo/wordmark.svg` | The editor's splash, drawn by the engine's SVG importer (light / dark) |
| `logo/mark-ember*.svg` | README logo (light / dark) |
| the rest | Kept for the docs site (8.4) and exports |

The fonts (Outfit, Figtree, JetBrains Mono, SIL OFL) are not here: the kit only has
`.woff2`, so CMake fetches the TTF releases (`cmake/Dependencies.cmake`).
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(kit: Path, *args: str) -> str:
    out = subprocess.run(["git", "-C", str(kit), *args], capture_output=True, text=True, check=False)
    if out.returncode != 0:
        sys.exit(f"sync_brand: git {' '.join(args)} failed in {kit}: {out.stderr.strip()}")
    return out.stdout.strip()


def source_text(kit: Path) -> tuple[str, bool]:
    commit = git(kit, "rev-parse", "HEAD")
    date = git(kit, "log", "-1", "--format=%cI")
    subject = git(kit, "log", "-1", "--format=%s")
    # Files copied with uncommitted changes in the kit would not be reproducible
    # from the recorded commit: mark them (and refuse to write, below).
    dirty = git(kit, "status", "--porcelain", "--", *FILES.keys())
    lines = [
        "Cuervo design kit, synced by tools/sync_brand.py. Do not edit; run the script.",
        f"kit commit: {commit}",
        f"kit commit date: {date}",
        f"kit commit subject: {subject}",
        f"kit working tree: {'DIRTY' if dirty else 'clean'} for the copied files",
        "",
        "files (sha256  path under assets/brand  <-  kit path):",
    ]
    for src, dst in sorted(FILES.items(), key=lambda kv: kv[1]):
        lines.append(f"{sha256(kit / src)}  {dst}  <-  {src}")
    return "\n".join(lines) + "\n", bool(dirty)


def verify() -> int:
    """Without the kit: every file matches its SHA-256 in SOURCE.txt, nothing is
    missing and nothing else is there (catches hand edits; the ctest)."""
    source = DEST / "SOURCE.txt"
    if not source.is_file():
        print("sync_brand: assets/brand/SOURCE.txt is missing", file=sys.stderr)
        return 1
    listed = {}
    for line in source.read_text().splitlines():
        parts = line.split("  ")
        if len(parts) == 4 and parts[2] == "<-" and len(parts[0]) == 64:
            listed[parts[1]] = parts[0]
    errors = [f"{d}: missing" for d in listed if not (DEST / d).is_file()]
    errors += [f"{d}: edited (SHA-256 differs from SOURCE.txt)" for d, h in listed.items()
               if (DEST / d).is_file() and sha256(DEST / d) != h]
    if (DEST / "README.md").read_bytes() != README.encode():
        errors.append("README.md: edited (the script writes it)")
    owned = set(listed) | {"SOURCE.txt", "README.md"}
    errors += [f"{p.relative_to(DEST).as_posix()}: not from the kit" for p in DEST.rglob("*")
               if p.is_file() and p.relative_to(DEST).as_posix() not in owned]
    if set(listed) != set(FILES.values()):
        errors.append("SOURCE.txt lists other files than tools/sync_brand.py copies; re-run the sync")
    for e in errors:
        print(f"assets/brand/{e}")
    if errors:
        print("sync_brand: assets/brand/ was changed without the sync script", file=sys.stderr)
        return 1
    print(f"sync_brand: assets/brand/ matches SOURCE.txt ({len(listed)} files)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kit", default=os.environ.get("CUERVO_DESIGN_KIT", DEFAULT_KIT))
    parser.add_argument("--check", action="store_true", help="compare only; exit 1 when out of date")
    parser.add_argument("--verify", action="store_true",
                        help="no kit needed: check assets/brand/ against the hashes in SOURCE.txt")
    args = parser.parse_args()
    if args.verify:
        return verify()
    kit = Path(args.kit).resolve()
    missing = [src for src in FILES if not (kit / src).is_file()]
    if missing:
        print(f"sync_brand: not a design kit (missing {', '.join(missing)}): {kit}", file=sys.stderr)
        return 1

    source, dirty = source_text(kit)
    expected = {dst: (kit / src).read_bytes() for src, dst in FILES.items()}
    expected["SOURCE.txt"] = source.encode()
    expected["README.md"] = README.encode()

    # Everything under assets/brand/ is owned by this script: stray files are
    # removed (a file dropped from FILES disappears from the repo too).
    present = {p.relative_to(DEST).as_posix() for p in DEST.rglob("*") if p.is_file()} if DEST.exists() else set()
    stale = sorted(present - expected.keys())
    changed = sorted(d for d, data in expected.items() if not (DEST / d).is_file() or (DEST / d).read_bytes() != data)

    if args.check:
        for d in changed:
            print(f"out of date: assets/brand/{d}")
        for d in stale:
            print(f"not from the kit: assets/brand/{d}")
        if changed or stale:
            print("sync_brand: assets/brand/ differs from the kit; run python3 -I tools/sync_brand.py", file=sys.stderr)
            return 1
        print(f"sync_brand: assets/brand/ matches the kit ({len(FILES)} files)")
        return 0

    if dirty:
        print(f"sync_brand: the kit has uncommitted changes to files it would copy; commit them in {kit} first",
              file=sys.stderr)
        return 1
    for d in stale:
        (DEST / d).unlink()
    for d in changed:
        target = DEST / d
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(expected[d])
    for d in sorted((p for p in DEST.rglob("*") if p.is_dir()), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()
    print(f"sync_brand: {len(changed)} file(s) written, {len(stale)} removed, from kit "
          f"{source.splitlines()[1].split(': ')[1][:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
