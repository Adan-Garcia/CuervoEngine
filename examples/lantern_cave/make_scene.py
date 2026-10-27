#!/usr/bin/env python3
"""Writes authoring/cave.txt (the level as scene text) and builds scenes/cave.scene.

The cave is drawn below as rectangles of rock on an 80x22 grid of 32-pixel
tiles; everything else (lights, crates, gems, the explorer) is placed in tile
coordinates. Run it after changing the layout:
    python3 examples/lantern_cave/make_scene.py [path/to/cuervo-tools]
"""

import os
import subprocess
import sys

TILE = 32
W, H = 80, 22

ROCK = (0.36, 0.3, 0.4)

# (x0, y0, x1, y1) inclusive rock rectangles, in tiles.
ROCK_RECTS = [
    (0, 0, W - 1, 1),        # ceiling
    (0, 19, W - 1, H - 1),   # floor
    (0, 2, 1, 18),           # left wall
    (78, 2, 79, 18),         # right wall
    (14, 15, 15, 18),        # pillar
    (20, 15, 27, 15),        # platform
    (42, 2, 43, 11),         # stalactite
    (48, 2, 58, 14),         # tunnel roof
    (61, 17, 62, 18),        # steps up to the ledge
    (63, 15, 64, 18),
    (65, 13, 66, 18),
    (67, 11, 77, 18),        # the ledge with the door
    (70, 2, 71, 4),          # the beacon's rock
]
# Holes cut back out of the rock (the pit).
HOLES = [(31, 19, 35, 20)]


def solid_cells():
    grid = [[False] * W for _ in range(H)]
    for x0, y0, x1, y1 in ROCK_RECTS:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                grid[y][x] = True
    for x0, y0, x1, y1 in HOLES:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                grid[y][x] = False
    return [(x, y) for y in range(H) for x in range(W) if grid[y][x]]


def color(r, g, b, a=1.0):
    return f"Color {{ r = {r}, g = {g}, b = {b}, a = {a} }}"


def hexcolor(h, a=1.0):
    return color(*(round(int(h[i:i + 2], 16) / 255.0, 4) for i in (0, 2, 4)), a)


def vec(x, y):
    return f"({float(x)}, {float(y)})"


def tile_center(tx, ty):
    return tx * TILE + TILE / 2, ty * TILE + TILE / 2


def indent(text, n):
    pad = " " * n
    return "\n".join(pad + line if line else line for line in text.splitlines())


def node(cls, name, props=(), children=()):
    lines = [f'node {cls} "{name}" {{']
    for key, typ, val in props:
        lines.append(f"    {key}: {typ} = {val}")
    for child in children:
        lines.append(indent(child, 4))
    lines.append("}")
    return "\n".join(lines)


def light(name, x, y, radius, col, energy=1.0, softness=16.0, shadows=True, rotation=None, spot=None,
          shadow_alpha=0.65):
    props = [("position", "vec2", vec(x, y)), ("radius", "float", float(radius)), ("color", "Color", col),
             ("energy", "float", float(energy)), ("shadow_softness", "float", float(softness)),
             ("shadow_enabled", "bool", "true" if shadows else "false"),
             ("shadow_color", "Color", color(0.0, 0.0, 0.0, shadow_alpha))]
    if rotation is not None:
        props.append(("rotation", "float", float(rotation)))
    if spot is not None:
        props.append(("spot_angle", "float", float(spot)))
    return node("PointLight2D", name, props)


def shape(name, kind, x, y, fill, radius=None, size=None, rotation=None, corner=None):
    props = [("kind", "ShapeKind", f"ShapeKind.{kind}"), ("position", "vec2", vec(x, y)), ("fill", "Color", fill)]
    if radius is not None:
        props.append(("radius", "float", float(radius)))
    if size is not None:
        props.append(("size", "vec2", vec(*size)))
    if rotation is not None:
        props.append(("rotation", "float", float(rotation)))
    if corner is not None:
        props.append(("corner_radius", "float", float(corner)))
    return node("Shape", name, props)


def collision(kind, size=None, radius=None):
    props = [("shape_type", "int", str(kind))]
    if size is not None:
        props.append(("size", "vec2", vec(*size)))
    if radius is not None:
        props.append(("radius", "float", float(radius)))
    return node("CollisionShape2D", "Shape", props)


def terrain():
    cells = ", ".join(f"TileCell {{ x = {x}, y = {y}, tile_id = 0, terrain = 0 }}" for x, y in solid_cells())
    tileset = (f"TileSet {{ tile_size = {vec(TILE, TILE)}, tiles = [TileDef {{ id = 0, asset = \"\", "
               f"color = {color(*ROCK)}, walkable = false, nav_cost = 1.0, terrain = 0, peering_bits = 0 }}] }}")
    return node("TileMapLayer", "Rock", [
        ("tile_size", "vec2", vec(TILE, TILE)),
        ("tileset", "TileSet", tileset),
        ("cells", "List<TileCell>", f"[{cells}]"),
        ("light_occluders", "bool", "true"),
    ], [node("TileMapCollision2D", "Collision")])


def explorer(tx, ty):
    x, _ = tile_center(tx, ty)
    y = (ty + 1) * TILE - 26
    dark = hexcolor("2b2433")
    leg = lambda side: node("Node2D", f"Hip{side}", [("position", "vec2", vec(-5 if side == "L" else 5, 8))], [
        shape("Leg", "Capsule", 0, 9, dark, radius=3.5, size=(14, 0), rotation=1.5708)])
    torso = node("Shape", "Torso", [
        ("kind", "ShapeKind", "ShapeKind.Rect"), ("position", "vec2", vec(0, -6)), ("size", "vec2", vec(20, 28)),
        ("corner_radius", "float", "6.0"), ("fill", "Color", hexcolor("4f7cff"))], [
        shape("Head", "Circle", 0, -22, hexcolor("f1d2b0"), radius=10),
        shape("Eye", "Circle", 4, -23, dark, radius=2.2),
        shape("Hat", "Rect", 0, -31, hexcolor("8a3b2e"), size=(22, 6), corner=2)])
    arm = node("Node2D", "Arm", [("position", "vec2", vec(7, -12))], [
        shape("Sleeve", "Capsule", 0, 8, hexcolor("3d62d6"), radius=3.0, size=(14, 0), rotation=1.5708),
        node("Node2D", "Lantern", [("position", "vec2", vec(0, 19))], [
            shape("Glass", "Circle", 0, 0, hexcolor("ffd27a"), radius=5),
            light("Light", 0, 0, 300, color(1.0, 0.86, 0.6), energy=1.1, softness=10.0)])])
    return node("Explorer", "Explorer", [("position", "vec2", vec(x, y))], [
        collision(1, size=(26, 52)),
        node("Camera2D", "Camera"),
        node("Node2D", "Visual", [], [leg("L"), leg("R"), torso, arm]),
        node("AnimationPlayer", "Anim"),
    ])


def torch(name, tx, ty):
    x, y = tile_center(tx, ty)
    return node("Torch", name, [("position", "vec2", vec(x, y))], [
        shape("Flame", "Circle", 0, 0, hexcolor("ffb347"), radius=6),
        light("Light", 0, 0, 340, color(1.0, 0.62, 0.3), energy=1.0, softness=14.0, shadow_alpha=0.75),
        node("AnimationPlayer", "Flicker"),
    ])


def crate(name, tx):
    x, _ = tile_center(tx, 18)
    return node("Crate", name, [("position", "vec2", vec(x, 19 * TILE - 20)), ("mass", "float", "3.0")], [
        collision(1, size=(40, 40)),
        node("LightOccluder2D", "Occluder", [("size", "vec2", vec(40, 40))]),
    ])


def gem(name, x, y):
    return node("Gem", name, [("position", "vec2", vec(x, y))], [
        collision(2, radius=14),
        light("Glow", 0, 0, 80, color(0.45, 0.9, 1.0), energy=0.7, shadows=False),
    ])


def scene_text():
    level_w, level_h = W * TILE, H * TILE
    parts = [
        shape("Backdrop", "Rect", level_w / 2, level_h / 2, hexcolor("2a2230"), size=(level_w, level_h)),
        node("AmbientLight2D", "Darkness", [("color", "Color", color(0.03, 0.02, 0.06)),
                                            ("darkness", "float", "0.86"), ("glow", "float", "0.3")]),
        terrain(),
        torch("Torch1", 6, 15),
        torch("Torch2", 24, 12),
        torch("Torch3", 53, 16),
        torch("Torch4", 73, 8),
        light("Crystal", 43 * TILE, 12 * TILE + 10, 380, color(0.45, 0.6, 1.0), energy=1.2, softness=8.0,
              rotation=1.5708, spot=1.1),
        node("Beacon", "Beacon", [("position", "vec2", vec(71 * TILE, 5 * TILE + 10))], [
            light("Beam", 0, 0, 640, color(0.85, 0.95, 1.0), energy=1.0, softness=6.0, spot=0.55)]),
        crate("Crate1", 38),
        crate("Crate2", 40),
        gem("Gem1", 24 * TILE, 13 * TILE + 24),
        gem("Gem2", 33 * TILE + 16, 20 * TILE),
        gem("Gem3", 54 * TILE, 17 * TILE + 8),
        gem("Gem4", 69 * TILE, 10 * TILE),
        node("ExitDoor", "Door", [("position", "vec2", vec(76 * TILE, 11 * TILE - 40))], [
            collision(1, size=(48, 80)),
            light("DoorLight", 0, 0, 140, color(0.5, 1.0, 0.7), energy=0.5, shadows=False)]),
        explorer(4, 18),
    ]
    return node("Node2D", "Cave", [], parts) + "\n"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    tools = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        here, "..", "..", "build", "debug", "tools", "cuervo_tools", "cuervo-tools")
    os.makedirs(os.path.join(here, "authoring"), exist_ok=True)
    os.makedirs(os.path.join(here, "scenes"), exist_ok=True)
    txt = os.path.join(here, "authoring", "cave.txt")
    with open(txt, "w", encoding="utf-8") as f:
        f.write(scene_text())
    subprocess.run([tools, "scene", "build", txt, os.path.join(here, "scenes", "cave.scene")], check=True)


if __name__ == "__main__":
    main()
