#!/usr/bin/env python3
"""Writes authoring/level.txt and builds scenes/*.scene."""

import os
import subprocess
import sys

SCENE_TEXT = """node Node2D "Level" {
    position: vec2 = (0.0, 0.0)
    node StaticBody2D "Floor" {
        position: vec2 = (640.0, 680.0)
        node CollisionShape2D "Shape" {
            shape_type: int = 1
            size: vec2 = (1280.0, 40.0)
        }
    }
    node StaticBody2D "WallLeft" {
        position: vec2 = (20.0, 360.0)
        node CollisionShape2D "Shape" {
            shape_type: int = 1
            size: vec2 = (40.0, 720.0)
        }
    }
    node StaticBody2D "WallRight" {
        position: vec2 = (1260.0, 360.0)
        node CollisionShape2D "Shape" {
            shape_type: int = 1
            size: vec2 = (40.0, 720.0)
        }
    }
    node StaticBody2D "Platform1" {
        position: vec2 = (400.0, 520.0)
        node CollisionShape2D "Shape" {
            shape_type: int = 1
            size: vec2 = (300.0, 30.0)
        }
    }
    node StaticBody2D "Platform2" {
        position: vec2 = (850.0, 380.0)
        node CollisionShape2D "Shape" {
            shape_type: int = 1
            size: vec2 = (300.0, 30.0)
        }
    }
    node Crate "Crate" {
        position: vec2 = (300.0, 620.0)
        mass: float = 2.0
        node CollisionShape2D "Shape" {
            shape_type: int = 1
            size: vec2 = (40.0, 40.0)
        }
    }
    node Coin "Coin" {
        position: vec2 = (850.0, 340.0)
        node CollisionShape2D "Shape" {
            shape_type: int = 2
            radius: float = 16.0
        }
    }
    node Player "Player" {
        position: vec2 = (150.0, 600.0)
        node CollisionShape2D "Shape" {
            shape_type: int = 1
            size: vec2 = (32.0, 56.0)
        }
        node RayCast2D "Ray" {
            target_position: vec2 = (0.0, 40.0)
        }
    }
}
"""

script_dir = os.path.dirname(os.path.abspath(__file__))
authoring_path = os.path.join(script_dir, "authoring", "level.txt")
scene_path = os.path.join(script_dir, "scenes", "level.scene")

with open(authoring_path, "w") as f:
    f.write(SCENE_TEXT)

print(f"Wrote {authoring_path}")

tools = os.path.abspath(os.path.join(script_dir, "..", "..", "build", "debug", "tools", "cuervo_tools", "cuervo-tools"))
if os.path.exists(tools):
    scenes = ["level", "joints", "vehicles", "sandbox"]
    for s in scenes:
        src = os.path.join(script_dir, "authoring", f"{s}.txt")
        dst = os.path.join(script_dir, "scenes", f"{s}.scene")
        if os.path.exists(src):
            subprocess.run([tools, "scene", "build", src, dst], check=True)
            print(f"Built {dst}")
