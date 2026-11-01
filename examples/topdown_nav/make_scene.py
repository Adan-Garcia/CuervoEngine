#!/usr/bin/env python3
"""Writes authoring/level.txt, the text view of scenes/level.scene."""

import os
import subprocess
import sys

# Level dimensions: 20 x 11 tiles of 64x64 virtual pixels
WIDTH = 20
HEIGHT = 11

# 0: Ground (walkable), 1: Wall (non-walkable)
grid = [[0 for _ in range(WIDTH)] for _ in range(HEIGHT)]

# Border walls
for x in range(WIDTH):
    grid[0][x] = 1
    grid[HEIGHT - 1][x] = 1
for y in range(HEIGHT):
    grid[y][0] = 1
    grid[y][WIDTH - 1] = 1

# Vertical wall at x = 6 (y = 1..9), with a door at y = 5
for y in range(1, HEIGHT - 1):
    if y != 5:
        grid[y][6] = 1

# Vertical wall at x = 13 (y = 1..9), with a door at y = 3
for y in range(1, HEIGHT - 1):
    if y != 3:
        grid[y][13] = 1

# Center room obstacles: pillars at (9, 4), (10, 4), (9, 6), (10, 6)
grid[4][9] = 1
grid[4][10] = 1
grid[6][9] = 1
grid[6][10] = 1

# Build cells list
cells = []
for y in range(HEIGHT):
    for x in range(WIDTH):
        tile_id = grid[y][x]
        cells.append(
            f"TileCell {{ x = {x}, y = {y}, tile_id = {tile_id}, terrain = {tile_id} }}"
        )

tiles = [
    "TileDef { id = 0, asset = \"\", color = Color { r = 0.1607843137254902, g = 0.20392156862745098, b = 0.30196078431372547, a = 1.0 }, walkable = true, nav_cost = 1.0, terrain = 0, peering_bits = 0 }",
    "TileDef { id = 1, asset = \"\", color = Color { r = 0.5490196078431373, g = 0.25098039215686274, b = 0.25098039215686274, a = 1.0 }, walkable = false, nav_cost = 1.0, terrain = 1, peering_bits = 0 }",
]

tileset_str = (
    "TileSet { tile_size = (64.0, 64.0), tiles = [" + ", ".join(tiles) + "] }"
)
cells_str = "[" + ", ".join(cells) + "]"

out = [
    'node Node2D "Level" {',
    "    position: vec2 = (0.0, 0.0)",
    '    node TileMapLayer "Terrain" {',
    "        tile_size: vec2 = (64.0, 64.0)",
    f"        tileset: TileSet = {tileset_str}",
    f"        cells: List<TileCell> = {cells_str}",
    "    }",
    '    node NavigationRegion2D "NavRegion" {',
    "        cell_size: vec2 = (64.0, 64.0)",
    "        allow_diagonal: bool = true",
    "    }",
    '    node Character "Character" {',
    "        position: vec2 = (224.0, 352.0)",
    '        node NavigationAgent2D "Agent" {',
    "            path_desired_distance: float = 8.0",
    "            target_desired_distance: float = 8.0",
    "            max_speed: float = 240.0",
    "        }",
    "    }",
    "}",
]

here = os.path.dirname(os.path.abspath(__file__))
target = os.path.join(here, "authoring", "level.txt")
os.makedirs(os.path.dirname(target), exist_ok=True)
with open(target, "w") as fh:
    fh.write("\n".join(out) + "\n")
print("wrote", os.path.relpath(target), file=sys.stderr)

# Build scene file if cuervo-tools binary is found
tools_bin = os.path.abspath(os.path.join(here, "..", "..", "build", "debug", "tools", "cuervo_tools", "cuervo-tools"))
if os.path.exists(tools_bin):
    scene_path = os.path.join(here, "scenes", "level.scene")
    subprocess.check_call([tools_bin, "scene", "build", target, scene_path])
    print("built", os.path.relpath(scene_path), file=sys.stderr)
