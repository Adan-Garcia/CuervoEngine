#!/usr/bin/env python3
"""Generates the showcase_duel art (original, drawn here from shapes; no third-party assets).

  python3 examples/showcase_duel/tools/make_art.py

Writes assets/art/*.png and assets/duel.atlas. Everything is drawn at 4x and averaged down, with a
fixed random seed, so the output is the same on every run. Needs Pillow."""
import json, os, random
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets", "art")
SS = 4


def canvas(w, h):
    img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def done(img, w, h):
    return img.resize((w, h), Image.LANCZOS)


def ship(hull, glow, flame):
    """A 2-frame sheet (64x32): a pointed ship facing right, the thrust flame longer on frame 1."""
    sheet = Image.new("RGBA", (64, 32))
    for f in range(2):
        img, d = canvas(32, 32)
        s = SS
        tail = 6 - (4 if f == 0 else 6)
        d.polygon([(8 * s, 12 * s), (tail * s, 16 * s), (8 * s, 20 * s)], fill=flame + (255,))
        d.polygon([(8 * s, 14 * s), ((tail + 3) * s, 16 * s), (8 * s, 18 * s)], fill=(255, 250, 220, 255))
        d.polygon([(8 * s, 6 * s), (28 * s, 16 * s), (8 * s, 26 * s), (12 * s, 16 * s)],
                  fill=hull + (255,), outline=(255, 255, 255, 255))
        d.polygon([(14 * s, 12 * s), (22 * s, 16 * s), (14 * s, 20 * s)], fill=glow + (255,))
        sheet.paste(done(img, 32, 32), (32 * f, 0))
    return sheet


def bullet():
    img, d = canvas(12, 12)
    s = SS
    d.ellipse([1 * s, 1 * s, 11 * s, 11 * s], fill=(255, 255, 255, 90))
    d.ellipse([3 * s, 3 * s, 9 * s, 9 * s], fill=(255, 255, 255, 255))
    return done(img, 12, 12)


def spark():
    img, d = canvas(16, 16)
    s = SS
    d.line([(8 * s, 1 * s), (8 * s, 15 * s)], fill=(255, 220, 140, 255), width=2 * s)
    d.line([(1 * s, 8 * s), (15 * s, 8 * s)], fill=(255, 220, 140, 255), width=2 * s)
    d.ellipse([4 * s, 4 * s, 12 * s, 12 * s], fill=(255, 255, 255, 255))
    return done(img, 16, 16)


def panel():
    """A 24x24 9-slice frame: 8 px borders with a bevel, a translucent dark middle."""
    img, d = canvas(24, 24)
    s = SS
    d.rounded_rectangle([0, 0, 24 * s - 1, 24 * s - 1], radius=7 * s, fill=(16, 22, 44, 215),
                        outline=(127, 230, 255, 255), width=2 * s)
    d.rounded_rectangle([3 * s, 3 * s, 21 * s - 1, 21 * s - 1], radius=4 * s, outline=(60, 90, 140, 255), width=s)
    return done(img, 24, 24)


def button():
    img, d = canvas(24, 24)
    s = SS
    d.rounded_rectangle([0, 0, 24 * s - 1, 24 * s - 1], radius=8 * s, fill=(214, 128, 44, 235),
                        outline=(255, 240, 210, 255), width=2 * s)
    d.rounded_rectangle([3 * s, 3 * s, 21 * s - 1, 12 * s], radius=5 * s, fill=(255, 190, 120, 70))
    return done(img, 24, 24)


def background(w=640, h=360):
    rnd = random.Random(20261103)
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = (int(7 + 14 * t), int(10 + 10 * t), int(24 + 40 * t), 255)
        for x in range(w):
            px[x, y] = c
    d = ImageDraw.Draw(img)
    for _ in range(140):
        x, y = rnd.randrange(w), rnd.randrange(h)
        v = rnd.choice((90, 140, 200, 255))
        d.point((x, y), fill=(v, v, min(255, v + 30), 255))
    for _ in range(5):  # a few soft planets
        x, y, r = rnd.randrange(60, w - 60), rnd.randrange(40, h - 40), rnd.randrange(14, 34)
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(layer).ellipse([x - r, y - r, x + r, y + r], fill=(40, 60, 120, 120))
        img = Image.alpha_composite(img, layer.filter(ImageFilter.GaussianBlur(6)))
    # An arena rim, so the playfield's edge reads under the bloom.
    ImageDraw.Draw(img).rectangle([8, 8, w - 9, h - 9], outline=(40, 70, 120, 255))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    ship((127, 230, 255), (20, 60, 100), (80, 160, 255)).save(os.path.join(OUT, "ship_a.png"))
    ship((255, 184, 107), (110, 50, 10), (255, 110, 50)).save(os.path.join(OUT, "ship_b.png"))
    bullet().save(os.path.join(OUT, "bullet.png"))
    spark().save(os.path.join(OUT, "spark.png"))
    panel().save(os.path.join(OUT, "panel.png"))
    button().save(os.path.join(OUT, "button.png"))
    background().save(os.path.join(OUT, "bg.png"))
    spec = {"max_size": 1024, "padding": 1, "extrude": True,
            "sprites": [{"path": "assets/art/%s.png" % n, "name": n} for n in ("bullet", "spark", "panel", "button", "bg")],
            "sheets": [{"image": "assets/art/ship_a.png", "frame_w": 32, "frame_h": 32, "prefix": "ship_a"},
                       {"image": "assets/art/ship_b.png", "frame_w": 32, "frame_h": 32, "prefix": "ship_b"}]}
    with open(os.path.join(OUT, "..", "duel.atlas"), "w") as f:
        json.dump(spec, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
