#!/usr/bin/env python3
"""Writes a deterministic, deliberately heavy SVG for vector renderer perf tests.

    python3 tools/gen_complex_svg.py out.svg [shapes]

Thousands of curved paths (cubic + quadratic + arcs) with fills (both rules),
strokes (all joins/caps, dashes), linear + radial gradients, group transforms
and a few clip paths. Fixed seed: the same file every run.
"""
import math
import random
import sys

out = sys.argv[1]
count = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
rng = random.Random(0x5EED)
W, H = 1600, 900


def color():
    return "#%02x%02x%02x" % (rng.randrange(256), rng.randrange(256), rng.randrange(256))


def blob(cx, cy, r, points):
    # A closed curvy blob of cubic segments.
    pts = []
    for i in range(points):
        a = 2 * math.pi * i / points
        rr = r * rng.uniform(0.6, 1.2)
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d = "M%.2f %.2f" % pts[0]
    for i in range(points):
        p0, p1 = pts[i], pts[(i + 1) % points]
        k = r * 0.4
        a0 = 2 * math.pi * i / points + math.pi / 2
        a1 = 2 * math.pi * (i + 1) / points + math.pi / 2
        d += " C%.2f %.2f %.2f %.2f %.2f %.2f" % (p0[0] + k * math.cos(a0), p0[1] + k * math.sin(a0),
                                                p1[0] - k * math.cos(a1), p1[1] - k * math.sin(a1), p1[0], p1[1])
    return d + " Z"


lines = ['<?xml version="1.0" encoding="UTF-8"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (W, H, W, H),
         "<defs>"]
gradients = 40
for i in range(gradients):
    stops = "".join('<stop offset="%.2f" stop-color="%s"/>' % (j / 3, color()) for j in range(4))
    if i % 2:
        lines.append('<linearGradient id="g%d" x1="0" y1="0" x2="1" y2="1">%s</linearGradient>' % (i, stops))
    else:
        lines.append('<radialGradient id="g%d" fx="0.3" fy="0.3">%s</radialGradient>' % (i, stops))
for i in range(4):
    lines.append('<clipPath id="c%d"><path d="%s"/></clipPath>' % (i, blob(rng.uniform(200, W - 200),
                                                                         rng.uniform(200, H - 200), 220, 9)))
lines.append("</defs>")

joins = ["miter", "round", "bevel"]
caps = ["butt", "round", "square"]
for i in range(count):
    cx, cy = rng.uniform(0, W), rng.uniform(0, H)
    r = rng.uniform(6, 40)
    kind = rng.random()
    if kind < 0.45:
        fill = "url(#g%d)" % rng.randrange(gradients) if rng.random() < 0.3 else color()
        attrs = 'fill="%s" fill-opacity="%.2f"' % (fill, rng.uniform(0.5, 1.0))
        if rng.random() < 0.5:
            attrs += ' stroke="%s" stroke-width="%.1f" stroke-linejoin="%s"' % (color(), rng.uniform(0.5, 4),
                                                                              rng.choice(joins))
        d = blob(cx, cy, r, rng.randrange(4, 10))
    elif kind < 0.75:
        d = "M%.1f %.1f" % (cx, cy)
        for _ in range(rng.randrange(2, 6)):
            d += " Q%.1f %.1f %.1f %.1f" % (cx + rng.uniform(-60, 60), cy + rng.uniform(-60, 60),
                                            cx + rng.uniform(-60, 60), cy + rng.uniform(-60, 60))
        attrs = 'fill="none" stroke="%s" stroke-width="%.1f" stroke-linecap="%s" stroke-linejoin="%s"' % (
            color(), rng.uniform(1, 8), rng.choice(caps), rng.choice(joins))
        if rng.random() < 0.3:
            attrs += ' stroke-dasharray="%d %d"' % (rng.randrange(2, 12), rng.randrange(2, 8))
    else:
        # Self-intersecting star with an arc, both fill rules.
        n = rng.choice([5, 7, 9])
        pts = [(cx + r * math.cos(2 * math.pi * k * 2 / n), cy + r * math.sin(2 * math.pi * k * 2 / n))
               for k in range(n)]
        d = "M" + " L".join("%.1f %.1f" % p for p in pts) + " A%.1f %.1f 0 0 1 %.1f %.1f Z" % (r, r, *pts[0])
        attrs = 'fill="%s" fill-rule="%s"' % (color(), rng.choice(["evenodd", "nonzero"]))
    clip = ' clip-path="url(#c%d)"' % rng.randrange(4) if rng.random() < 0.05 else ""
    transform = ' transform="rotate(%.1f %.1f %.1f)"' % (rng.uniform(0, 360), cx, cy) if rng.random() < 0.2 else ""
    lines.append('<path d="%s" %s%s%s/>' % (d, attrs, clip, transform))
lines.append("</svg>")
with open(out, "w") as f:
    f.write("\n".join(lines) + "\n")
