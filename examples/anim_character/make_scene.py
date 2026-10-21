#!/usr/bin/env python3
"""Writes authoring/character.txt, the text view of scenes/character.scene.

The character is a Node2D hierarchy (torso, head, arms, legs as child Shapes)
with an AnimationPlayer holding idle / walk / jump and an AnimationTree state
machine over them. Animations live inside the scene file as the player's
`animations` field (docs/SCENES.md), so the way to author them without the
editor is the text view:

    python3 examples/anim_character/make_scene.py
    cuervo-tools scene build examples/anim_character/authoring/character.txt \
                             examples/anim_character/scenes/character.scene

(`cuervo-tools scene dump` prints the same text back; the ctest anim_character
checks that building this file reproduces character.scene byte for byte.)
All times are ticks at the project's 60 Hz.
"""

import os
import sys


def f(x):
    s = repr(float(x))
    return s


def vec(*xs):
    return "(" + ", ".join(f(x) for x in xs) + ")"


def color(hexstr):
    r, g, b = (int(hexstr[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    return "Color { r = %s, g = %s, b = %s, a = 1.0 }" % (f(r), f(g), f(b))


# ------------------------------------------------------------------ animations

def key(t, value=(0.0, 0.0, 0.0, 0.0), interp="Linear", ease="Sine", text=""):
    v = tuple(value) + (0.0,) * (4 - len(value))
    return ("AnimKey { time = %d, value = %s, text = \"%s\", interp = AnimInterp.%s, "
            "ease_kind = Ease.%s, curve = 1.0 }" % (t, vec(*v), text, interp, ease))


def track(path, keys, kind="Property"):
    return 'AnimTrack { path = "%s", kind = AnimTrackKind.%s, keys = [%s] }' % (path, kind, ", ".join(keys))


def animation(name, length, mode, tracks):
    return 'Animation { name = "%s", length = %d, mode = AnimLoop.%s, tracks = [%s] }' % (
        name, length, mode, ", ".join(tracks))


def eio(t, *v):
    return key(t, v, "EaseInOut", "Sine")


def const(path, *v):
    return track(path, [key(0, v, "Step")])


BLUE, TEAL, ORANGE = (0.31, 0.49, 1.0, 1.0), (0.24, 0.81, 0.81, 1.0), (1.0, 0.60, 0.24, 1.0)

IDLE = animation("idle", 120, "Loop", [
    track("Torso:position", [eio(0, 0, -70), eio(60, 0, -67), eio(120, 0, -70)]),
    const("Torso:rotation", 0.0),
    const("Torso:scale", 1.0, 1.0),
    track("Torso/Head:rotation", [eio(0, 0.0), eio(60, 0.08), eio(120, 0.0)]),
    track("Torso/ArmL:rotation", [eio(0, 0.08), eio(60, 0.0), eio(120, 0.08)]),
    track("Torso/ArmR:rotation", [eio(0, -0.08), eio(60, 0.0), eio(120, -0.08)]),
    const("LegL:rotation", 0.0),
    const("LegR:rotation", 0.0),
    const("Torso/TorsoShape:fill", *BLUE),
])

WALK = animation("walk", 40, "Loop", [
    track("Torso:position", [eio(0, 0, -67), eio(10, 0, -71), eio(20, 0, -67), eio(30, 0, -71), eio(40, 0, -67)]),
    const("Torso:rotation", 0.12),
    const("Torso:scale", 1.0, 1.0),
    track("Torso/Head:rotation", [eio(0, -0.10), eio(20, -0.16), eio(40, -0.10)]),
    track("Torso/ArmL:rotation", [eio(0, 0.75), eio(20, -0.75), eio(40, 0.75)]),
    track("Torso/ArmR:rotation", [eio(0, -0.75), eio(20, 0.75), eio(40, -0.75)]),
    track("LegL:rotation", [eio(0, -0.65), eio(20, 0.65), eio(40, -0.65)]),
    track("LegR:rotation", [eio(0, 0.65), eio(20, -0.65), eio(40, 0.65)]),
    const("Torso/TorsoShape:fill", *TEAL),
])

JUMP = animation("jump", 40, "None", [
    track("Torso:position", [eio(0, 0, -70), eio(5, 0, -60), eio(8, 0, -74), eio(32, 0, -72), eio(38, 0, -61), eio(40, 0, -70)]),
    track("Torso:rotation", [eio(0, 0.0), eio(8, -0.10), eio(32, 0.10), eio(40, 0.0)]),
    track("Torso:scale", [eio(0, 1.0, 1.0), eio(5, 1.14, 0.84), eio(8, 0.9, 1.12), eio(20, 1.0, 1.0),
                          eio(32, 1.0, 1.0), eio(38, 1.16, 0.82), eio(40, 1.0, 1.0)]),
    track("Torso/Head:rotation", [eio(0, 0.0), eio(8, -0.15), eio(32, 0.10), eio(40, 0.0)]),
    track("Torso/ArmL:rotation", [eio(0, 0.1), eio(5, 0.6), eio(10, 2.6), eio(30, 2.0), eio(38, 0.5), eio(40, 0.1)]),
    track("Torso/ArmR:rotation", [eio(0, -0.1), eio(5, -0.6), eio(10, -2.6), eio(30, -2.0), eio(38, -0.5), eio(40, -0.1)]),
    track("LegL:rotation", [eio(0, 0.0), eio(5, 0.5), eio(8, -0.2), eio(20, -0.4), eio(32, 0.3), eio(38, 0.5), eio(40, 0.0)]),
    track("LegR:rotation", [eio(0, 0.0), eio(5, -0.5), eio(8, 0.2), eio(20, 0.5), eio(32, -0.3), eio(38, -0.5), eio(40, 0.0)]),
    const("Torso/TorsoShape:fill", *ORANGE),
    track("", [key(8, text="launch")], kind="Event"),
])

STATES = [
    'AnimStateDef { name = "idle", animation = "idle", speed = 1.0 }',
    'AnimStateDef { name = "walk", animation = "walk", speed = 1.0 }',
    'AnimStateDef { name = "jump", animation = "jump", speed = 1.0 }',
]


def transition(src, dst, duration, conds=(), at_end=False, interp="Linear", ease="Sine"):
    cs = ", ".join('AnimCondition { param = "%s", op = AnimCondOp.%s, value = 0.0 }' % c for c in conds)
    return ('AnimTransition { from_state = "%s", to_state = "%s", conditions = [%s], duration = %d, '
            'interp = AnimInterp.%s, ease_kind = Ease.%s, curve = 1.0, at_end = %s }' % (
                src, dst, cs, duration, interp, ease, "true" if at_end else "false"))


TRANSITIONS = [
    transition("*", "jump", 4, [("jump", "Trigger")], interp="EaseOut", ease="Quad"),
    transition("jump", "walk", 6, [("moving", "IsTrue")], at_end=True),
    transition("jump", "idle", 6, at_end=True),
    transition("idle", "walk", 8, [("moving", "IsTrue")], interp="EaseInOut", ease="Sine"),
    transition("walk", "idle", 8, [("moving", "IsFalse")], interp="EaseInOut", ease="Sine"),
]

# ----------------------------------------------------------------------- nodes

out = []


def node(cls, name, fields, depth, children=None):
    pad = "    " * depth
    out.append('%snode %s "%s" {' % (pad, cls, name))
    for fname, ftype, value in fields:
        out.append("%s    %s: %s = %s" % (pad, fname, ftype, value))
    if children:
        children(depth + 1)
    out.append("%s}" % pad)


def pos(x, y):
    return ("position", "vec2", vec(x, y))


def shape(name, depth, kind, fill, x=0.0, y=0.0, radius=16.0, size=(32.0, 32.0), rot=0.0, corner=0.0):
    node("Shape", name, [
        pos(x, y),
        ("rotation", "float", f(rot)),
        ("kind", "ShapeKind", "ShapeKind." + kind),
        ("radius", "float", f(radius)),
        ("size", "vec2", vec(*size)),
        ("corner_radius", "float", f(corner)),
        ("fill", "Color", color(fill)),
    ], depth)


HALF_PI = 1.5707963267948966
SKIN, SUIT, LIMB, DARK = "f2c9a0", "4f7cff", "34406e", "1a1d2e"


def limb(depth, x, y, length, radius, fill):
    shape("Limb", depth, "Capsule", fill, x=0.0, y=length / 2.0 + 2.0, radius=radius, size=(length, 1.0), rot=HALF_PI)


def build(depth):
    def leg(name, x):
        node("Node2D", name, [pos(x, -62.0)], depth, lambda d: limb(d, 0.0, 0.0, 44.0, 8.0, LIMB))

    leg("LegL", -12.0)
    leg("LegR", 12.0)

    def torso(d):
        def arm(name, x):
            node("Node2D", name, [pos(x, -48.0)], d, lambda dd: limb(dd, 0.0, 0.0, 24.0, 7.0, SKIN))

        arm("ArmL", -27.0)
        shape("TorsoShape", d, "Rect", SUIT, x=0.0, y=-26.0, size=(46.0, 60.0), corner=12.0)

        def head(dd):
            shape("HeadShape", dd, "Circle", SKIN, y=-18.0, radius=20.0)
            shape("EyeL", dd, "Circle", DARK, x=-8.0, y=-20.0, radius=3.5)
            shape("EyeR", dd, "Circle", DARK, x=8.0, y=-20.0, radius=3.5)

        node("Node2D", "Head", [pos(0.0, -58.0)], d, head)
        arm("ArmR", 27.0)

    node("Node2D", "Torso", [pos(0.0, -70.0)], depth, torso)
    node("AnimationPlayer", "Anim", [
        ("animations", "List<Animation>", "[%s]" % ", ".join([IDLE, WALK, JUMP])),
        ("autoplay", "str", '""'),
    ], depth)
    node("AnimationTree", "Tree", [
        ("states", "List<AnimStateDef>", "[%s]" % ", ".join(STATES)),
        ("transitions", "List<AnimTransition>", "[%s]" % ", ".join(TRANSITIONS)),
        ("start_state", "str", '"idle"'),
    ], depth)


node("Character", "Character", [pos(0.0, 0.0)], 0, build)

target = os.path.join(os.path.dirname(os.path.abspath(__file__)), "authoring", "character.txt")
os.makedirs(os.path.dirname(target), exist_ok=True)
with open(target, "w") as fh:
    fh.write("\n".join(out) + "\n")
print("wrote", os.path.relpath(target), file=sys.stderr)
