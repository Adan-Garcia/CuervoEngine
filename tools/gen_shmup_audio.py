#!/usr/bin/env python3
"""Generates the shmup example's sounds (examples/shmup/assets/) procedurally.

Everything is synthesized from oscillators and seeded noise, so the output is
reproducible and the sounds carry no third-party license (see
examples/shmup/assets/AUDIO_LICENSE.txt). Standard library only.

    python3 tools/gen_shmup_audio.py [output_dir]
"""

import math
import random
import struct
import sys
import wave
from pathlib import Path

SFX_RATE = 44100
MUSIC_RATE = 22050


def write_wav(path: Path, samples, rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    peak = max(1e-9, max(abs(s) for s in samples))
    gain = min(1.0, 0.9 / peak)  # normalize down only, never clip
    frames = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * gain)) * 32767)) for s in samples)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(frames)
    print(f"{path}: {len(samples) / rate:.2f} s")


def env(t: float, attack: float, decay: float) -> float:
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


def square(phase: float, duty: float = 0.5) -> float:
    return 1.0 if (phase % 1.0) < duty else -1.0


def triangle(phase: float) -> float:
    p = phase % 1.0
    return 4.0 * p - 1.0 if p < 0.5 else 3.0 - 4.0 * p


class Lowpass:
    """One-pole low-pass filter."""

    def __init__(self) -> None:
        self.y = 0.0

    def __call__(self, x: float, cutoff: float, rate: int) -> float:
        a = 1.0 - math.exp(-2.0 * math.pi * cutoff / rate)
        self.y += a * (x - self.y)
        return self.y


def shoot() -> list:
    n = int(0.075 * SFX_RATE)
    out, phase = [], 0.0
    for i in range(n):
        t = i / SFX_RATE
        f = 1500.0 - 9000.0 * t  # quick downward chirp
        phase += f / SFX_RATE
        out.append(0.5 * square(phase, 0.25) * env(t, 0.002, 0.025))
    return out


def hit(rng: random.Random) -> list:
    n = int(0.06 * SFX_RATE)
    out, phase, lp = [], 0.0, Lowpass()
    for i in range(n):
        t = i / SFX_RATE
        phase += (950.0 - 6000.0 * t) / SFX_RATE
        noise = lp(rng.uniform(-1.0, 1.0), 5000.0, SFX_RATE)
        out.append((0.6 * triangle(phase) + 0.7 * noise) * env(t, 0.001, 0.015))
    return out


def explosion(rng: random.Random, seconds: float, low: float, cutoff: float) -> list:
    n = int(seconds * SFX_RATE)
    out, phase, lp, lp2 = [], 0.0, Lowpass(), Lowpass()
    for i in range(n):
        t = i / SFX_RATE
        x = t / seconds
        c = cutoff * (1.0 - x) ** 2 + 120.0
        noise = lp2(lp(rng.uniform(-1.0, 1.0), c, SFX_RATE), c, SFX_RATE)
        phase += (low * (1.0 - 0.6 * x)) / SFX_RATE
        thump = math.sin(2.0 * math.pi * phase) * math.exp(-t / (seconds * 0.25))
        out.append((2.2 * noise + 0.8 * thump) * env(t, 0.003, seconds * 0.3))
    return out


def bomb(rng: random.Random) -> list:
    seconds = 1.6
    n = int(seconds * SFX_RATE)
    out, phase, lp = [], 0.0, Lowpass()
    for i in range(n):
        t = i / SFX_RATE
        # a rising whoosh that turns into a falling boom
        f = 200.0 + 1200.0 * t / 0.25 if t < 0.25 else 1400.0 * math.exp(-(t - 0.25) * 3.0) + 50.0
        phase += f / SFX_RATE
        noise = lp(rng.uniform(-1.0, 1.0), 300.0 + f, SFX_RATE)
        tone = 0.35 * square(phase, 0.5) * math.exp(-t * 2.0)
        out.append((1.8 * noise + tone) * env(t, 0.05, 0.5))
    return out


def hurt() -> list:
    n = int(0.45 * SFX_RATE)
    out, phase = [], 0.0
    for i in range(n):
        t = i / SFX_RATE
        phase += (620.0 * math.exp(-t * 5.0) + 60.0) / SFX_RATE
        saw = 2.0 * (phase % 1.0) - 1.0
        out.append(0.6 * saw * env(t, 0.004, 0.15))
    return out


def stage_loop(rng: random.Random) -> list:
    """Four bars at 140 BPM in A minor; loops seamlessly (whole number of beats,
    every note ends before the loop point)."""
    bpm = 140.0
    beat = 60.0 / bpm
    beats = 16
    n = int(round(beats * beat * MUSIC_RATE))
    out = [0.0] * n

    def note_hz(semitones_from_a4: int) -> float:
        return 440.0 * 2.0 ** (semitones_from_a4 / 12.0)

    def add_note(start_beat: float, length_beats: float, hz: float, voice, level: float, decay: float) -> None:
        s = int(start_beat * beat * MUSIC_RATE)
        e = min(n, s + int(length_beats * beat * MUSIC_RATE))
        phase = 0.0
        for i in range(s, e):
            t = (i - s) / MUSIC_RATE
            phase += hz / MUSIC_RATE
            release = min(1.0, (e - i) / (0.01 * MUSIC_RATE))  # no clicks at note ends
            out[i] += level * voice(phase) * env(t, 0.005, decay) * release

    # Bass: eighth-note roots, i - VI - VII - i (Am F G Am).
    roots = [-24, -28, -26, -24]
    for bar, root in enumerate(roots):
        for k in range(8):
            octave = 12 if k % 2 else 0
            add_note(bar * 4 + k * 0.5, 0.45, note_hz(root + octave), triangle, 0.55, 0.25)

    # Lead: a short arpeggiated motif per bar, pulse wave.
    motifs = [[0, 3, 7, 12, 7, 3, 7, 10], [-4, 0, 3, 8, 3, 0, 3, 7], [-2, 2, 5, 10, 5, 2, 5, 9], [0, 3, 7, 12, 15, 12, 7, 3]]
    for bar, motif in enumerate(motifs):
        for k, semi in enumerate(motif):
            add_note(bar * 4 + k * 0.5, 0.4, note_hz(semi), lambda p: square(p, 0.3), 0.16, 0.18)

    # Hi-hat (seeded noise) on eighths, kick on beats.
    for k in range(beats * 2):
        s = int(k * 0.5 * beat * MUSIC_RATE)
        lp = Lowpass()
        for i in range(s, min(n, s + int(0.04 * MUSIC_RATE))):
            t = (i - s) / MUSIC_RATE
            x = rng.uniform(-1.0, 1.0)
            out[i] += 0.12 * (x - lp(x, 3000.0, MUSIC_RATE)) * math.exp(-t / 0.012)
    for k in range(beats):
        s = int(k * beat * MUSIC_RATE)
        phase = 0.0
        for i in range(s, min(n, s + int(0.18 * MUSIC_RATE))):
            t = (i - s) / MUSIC_RATE
            phase += (40.0 + 110.0 * math.exp(-t * 30.0)) / MUSIC_RATE
            out[i] += 0.7 * math.sin(2.0 * math.pi * phase) * math.exp(-t / 0.07)
    return out


def main() -> None:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "examples/shmup/assets"
    rng = random.Random(20261015)
    write_wav(root / "sfx/shoot.wav", shoot(), SFX_RATE)
    write_wav(root / "sfx/hit.wav", hit(rng), SFX_RATE)
    write_wav(root / "sfx/explosion.wav", explosion(rng, 0.7, 90.0, 3500.0), SFX_RATE)
    write_wav(root / "sfx/big_explosion.wav", explosion(rng, 1.8, 60.0, 2500.0), SFX_RATE)
    write_wav(root / "sfx/bomb.wav", bomb(rng), SFX_RATE)
    write_wav(root / "sfx/hurt.wav", hurt(), SFX_RATE)
    write_wav(root / "music/stage_loop.wav", stage_loop(rng), MUSIC_RATE)


if __name__ == "__main__":
    main()
