#!/usr/bin/env python3
"""Writes a small animated GIF from PPM frames, and captures the frames of a game (milestone 11.5).

    python3 -I tools/ppm_gif.py capture --runtime build/release/runtime/cuervo \
        --game examples/shmup --press "..." --first 60 --last 300 --step 6 \
        --scale 2 --fps 20 -o docs/assets/shmup.gif

    python3 -I tools/ppm_gif.py encode frame1.ppm frame2.ppm ... -o out.gif [--scale 2] [--fps 20]

`capture` runs the runtime once per frame with `--screenshot --screenshot-tick <n>` (the runtime
simulates one tick per frame up to the tick, so the same arguments always give the same
pixels), then encodes the frames. The encoder is deterministic: a global palette from median
cut over a fixed sample, frame deltas as transparent pixels, plain LZW, no comment or
timestamp blocks, so the same frames always produce the same bytes. Standard library only.
"""

import argparse
import os
import subprocess
import sys
import tempfile


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 <w> <h> <max> then one whitespace byte, then the pixels.
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while data[pos : pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    if fields[0] != b"P6" or int(fields[3]) != 255:
        raise ValueError(f"{path}: not an 8-bit P6 PPM")
    w, h = int(fields[1]), int(fields[2])
    pixels = data[pos : pos + w * h * 3]
    if len(pixels) != w * h * 3:
        raise ValueError(f"{path}: truncated")
    return w, h, pixels


def downscale(w, h, pixels, factor):
    """Box filter by an integer factor."""
    if factor == 1:
        return w, h, pixels
    nw, nh = w // factor, h // factor
    out = bytearray(nw * nh * 3)
    n = factor * factor
    for y in range(nh):
        rows = [pixels[((y * factor + dy) * w) * 3 : ((y * factor + dy) * w + nw * factor) * 3] for dy in range(factor)]
        for x in range(nw):
            for c in range(3):
                total = 0
                for row in rows:
                    base = x * factor * 3 + c
                    total += sum(row[base : base + factor * 3 : 3])
                out[(y * nw + x) * 3 + c] = (total + n // 2) // n
    return nw, nh, bytes(out)


def median_cut(colors, count):
    """colors: list of (r, g, b) samples. Returns up to `count` palette entries."""
    boxes = [colors]
    while len(boxes) < count:
        # Split the box with the widest channel range (ties: the larger one, then the first).
        best, best_range, best_ch = None, 0, 0
        for i, box in enumerate(boxes):
            if len(box) < 2:
                continue
            for ch in range(3):
                vals = [c[ch] for c in box]
                span = max(vals) - min(vals)
                if span > best_range:
                    best, best_range, best_ch = i, span, ch
        if best is None:
            break
        box = sorted(boxes.pop(best), key=lambda c: (c[best_ch], c))
        mid = len(box) // 2
        boxes += [box[:mid], box[mid:]]
    palette = []
    for box in boxes:
        n = len(box)
        palette.append(tuple((sum(c[ch] for c in box) + n // 2) // n for ch in range(3)))
    return palette


def lzw(indices, min_bits):
    """GIF LZW of a bytes-like sequence of palette indices; returns the sub-block-less stream."""
    clear = 1 << min_bits
    end = clear + 1
    out = bytearray()
    acc = nbits = 0

    def emit(code, width):
        nonlocal acc, nbits
        acc |= code << nbits
        nbits += width
        while nbits >= 8:
            out.append(acc & 255)
            acc >>= 8
            nbits -= 8

    table = {}
    next_code = end + 1
    width = min_bits + 1
    emit(clear, width)
    prefix = indices[0]
    for k in indices[1:]:
        key = (prefix << 8) | k
        code = table.get(key)
        if code is not None:
            prefix = code
            continue
        emit(prefix, width)
        if next_code < 4096:
            table[key] = next_code
            if next_code == (1 << width) and width < 12:
                width += 1
            next_code += 1
        else:
            emit(clear, width)
            table = {}
            next_code = end + 1
            width = min_bits + 1
        prefix = k
    emit(prefix, width)
    # The decoder widens one code later than the encoder's table grows; keep the end code in step.
    emit(end, width)
    if nbits:
        out.append(acc & 255)
    return bytes(out)


def sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i : i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def encode_gif(frames, fps, path):
    """frames: list of (w, h, rgb bytes), all the same size."""
    w, h, _ = frames[0]
    # Palette: every 5th pixel of every frame; one entry is kept free for "unchanged".
    samples = []
    for _, _, px in frames:
        samples += [(px[i], px[i + 1], px[i + 2]) for i in range(0, len(px) - 2, 15)]
    palette = median_cut(samples, 255)
    transparent = len(palette)
    palette += [(0, 0, 0)] * (256 - len(palette))

    cache = {}

    def index_of(r, g, b):
        key = (r >> 2, g >> 2, b >> 2)
        v = cache.get(key)
        if v is None:
            best, best_d = 0, 1 << 30
            qr, qg, qb = (r & 0xFC) | 2, (g & 0xFC) | 2, (b & 0xFC) | 2
            for i, (pr, pg, pb) in enumerate(palette[:transparent]):
                d = (pr - qr) ** 2 + (pg - qg) ** 2 + (pb - qb) ** 2
                if d < best_d:
                    best, best_d = i, d
            v = cache[key] = best
        return v

    out = bytearray(b"GIF89a")
    out += w.to_bytes(2, "little") + h.to_bytes(2, "little")
    out += bytes([0xF7, 0, 0])  # global table of 256 colors
    for r, g, b in palette:
        out += bytes([r, g, b])
    out += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"  # loop forever
    delay = max(2, round(100 / fps))
    prev = None
    for n, (_, _, px) in enumerate(frames):
        idx = bytearray(w * h)
        for i in range(w * h):
            idx[i] = index_of(px[i * 3], px[i * 3 + 1], px[i * 3 + 2])
        if prev is not None:
            # A pixel the palette maps to the same entry as in the previous frame is left as it was.
            idx = bytearray(transparent if idx[i] == prev[i] else idx[i] for i in range(w * h))
            full = prev_full_apply(prev, idx, transparent)
        else:
            full = bytes(idx)
        prev = full
        out += bytes([0x21, 0xF9, 4, 0x05 if n else 0x04, delay & 255, delay >> 8, transparent if n else 0, 0])
        out += b"\x2c" + (0).to_bytes(2, "little") * 2 + w.to_bytes(2, "little") + h.to_bytes(2, "little") + b"\x00"
        out += bytes([8]) + sub_blocks(lzw(bytes(idx), 8))
    out += b"\x3b"
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def prev_full_apply(prev, delta, transparent):
    return bytes(p if d == transparent else d for p, d in zip(prev, delta))


def load_frames(paths, scale):
    frames = []
    for p in paths:
        w, h, px = read_ppm(p)
        frames.append(downscale(w, h, px, scale))
    return frames


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    enc = sub.add_parser("encode")
    enc.add_argument("frames", nargs="+")
    cap = sub.add_parser("capture")
    cap.add_argument("--runtime", required=True)
    cap.add_argument("--game", required=True)
    cap.add_argument("--press", default="")
    cap.add_argument("--first", type=int, default=60)
    cap.add_argument("--last", type=int, default=300)
    cap.add_argument("--step", type=int, default=6)
    cap.add_argument("--backend", default="gl")
    for p in (enc, cap):
        p.add_argument("-o", "--out", required=True)
        p.add_argument("--scale", type=int, default=2, help="integer downscale factor")
        p.add_argument("--fps", type=float, default=20)
    args = ap.parse_args(argv)

    if args.cmd == "encode":
        paths = args.frames
        tmp = None
    else:
        tmp = tempfile.TemporaryDirectory()
        paths = []
        env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen")
        for tick in range(args.first, args.last + 1, args.step):
            path = os.path.join(tmp.name, f"f{tick:05d}.ppm")
            cmd = [args.runtime, "--game", args.game, "--backend", args.backend, "--screenshot", path, "--screenshot-tick", str(tick)]
            if args.press:
                cmd += ["--press", args.press]
            r = subprocess.run(cmd, env=env, capture_output=True, text=True)
            if r.returncode != 0 or not os.path.exists(path):
                print(f"ppm_gif: capture of tick {tick} failed:\n{r.stdout}{r.stderr}", file=sys.stderr)
                return 1
            paths.append(path)
    size = encode_gif(load_frames(paths, args.scale), args.fps, args.out)
    print(f"ppm_gif: {len(paths)} frames, {size} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
