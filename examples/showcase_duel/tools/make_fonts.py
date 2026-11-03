#!/usr/bin/env python3
"""Subsets the two fallback fonts to the characters locale/strings.csv uses (plus digits, kana and
Arabic-Indic digits), so the example ships ~30 KB of font instead of 2 MB. Needs fontTools.

  make_fonts.py <NotoSansArabic-Regular.ttf> <MPLUS1p-Regular.ttf>

Both fonts are SIL OFL 1.1 (assets/fonts/LICENSES.txt); layout features are kept so shaping works."""
import csv, os, sys
from fontTools import subset

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")


def main():
    arabic_src, japanese_src = sys.argv[1], sys.argv[2]
    chars = set()
    with open(os.path.join(ROOT, "locale", "strings.csv"), encoding="utf-8") as f:
        for row in csv.DictReader(f):
            chars |= set(row["ar"]) | set(row["ja"])
    chars |= set("0123456789 ") | {chr(c) for c in range(0x660, 0x66A)}
    kana = {chr(c) for c in range(0x3041, 0x30FF)}
    for src, name, extra in ((arabic_src, "NotoSansArabic-subset.ttf", set()),
                             (japanese_src, "MPLUS1p-subset.ttf", kana)):
        opts = subset.Options()
        opts.layout_features = ["*"]
        font = subset.load_font(src, opts)
        sub = subset.Subsetter(opts)
        sub.populate(text="".join(sorted(chars | extra)))
        sub.subset(font)
        subset.save_font(font, os.path.join(ROOT, "assets", "fonts", name), opts)


if __name__ == "__main__":
    main()
