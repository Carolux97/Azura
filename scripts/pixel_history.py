#!/usr/bin/env python3
"""Reads a pixel history saved by the Metal backend's metal_pixel_history.

A history is two files with the same base name:
- <base>.txt: one line per guest draw of the frame, in order ("draw N sampled 0|1 rt0 WxH fmt F
  base B colormask ... blend ... depth ... vs ... ps ..." and the pixel shader's texture fetches,
  with their exponent bias "exp" and LOD bias "lod" in 1/32);
- <base>.half4: for every draw, a 64x36 grid of half-precision RGBA samples of render target 0
  after the draw (meaningful only for draws with "sampled 1").

Usage:
  pixel_history.py <base> list               draws that changed grid points of their render
                                             target since its previous sampled draw
  pixel_history.py <base> point <gx> <gy>    history of one grid point
  pixel_history.py <base> image <draw> <out.ppm> [scale]
                                             the grid after a draw, 8x upscaled, values times scale
"""

import struct
import sys

GRID_W, GRID_H = 64, 36
POINTS = GRID_W * GRID_H


def read_draws(base):
    draws = []
    with open(base + ".txt") as f:
        for line in f:
            line = line.rstrip("\n")
            rt = line[line.index(" rt0 ") + 5:line.index(" colormask ")]
            state = line[line.index(" colormask ") + 1:]
            draws.append({"sampled": " sampled 1 " in line, "rt": rt, "state": state})
    return draws


def read_grids(base, draw_count):
    with open(base + ".half4", "rb") as f:
        data = f.read()
    count = min(draw_count, len(data) // (POINTS * 8))
    return [struct.unpack_from("<%de" % (POINTS * 4), data, i * POINTS * 8) for i in range(count)]


def changes(draws, grids):
    """Yields (draw index, previous sampled draw index on the same render target or None)."""
    previous = {}
    for i, draw in enumerate(draws):
        if not draw["sampled"] or i >= len(grids):
            continue
        yield i, previous.get(draw["rt"])
        previous[draw["rt"]] = i


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    base, mode = sys.argv[1], sys.argv[2]
    draws = read_draws(base)
    grids = read_grids(base, len(draws))
    if mode == "list":
        for i, prev in changes(draws, grids):
            changed, box, before, after = 0, [GRID_W, GRID_H, -1, -1], 0.0, 0.0
            for p in range(POINTS):
                a = grids[i][p * 4:p * 4 + 3]
                b = grids[prev][p * 4:p * 4 + 3] if prev is not None else (0.0, 0.0, 0.0)
                if sum(abs(x - y) for x, y in zip(a, b)) < 0.004:
                    continue
                changed += 1
                x, y = p % GRID_W, p // GRID_W
                box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
                before += sum(b) / 3
                after += sum(a) / 3
            if changed:
                print(f"draw {i} rt {draws[i]['rt']} changed {changed} grid ({box[0]},{box[1]})-"
                      f"({box[2]},{box[3]}) lum {before / changed:.3f} -> {after / changed:.3f} | "
                      f"{draws[i]['state']}")
    elif mode == "point" and len(sys.argv) >= 5:
        p = int(sys.argv[4]) * GRID_W + int(sys.argv[3])
        for i, prev in changes(draws, grids):
            a = grids[i][p * 4:p * 4 + 4]
            b = grids[prev][p * 4:p * 4 + 4] if prev is not None else (0.0,) * 4
            if sum(abs(x - y) for x, y in zip(a[:3], b[:3])) < 0.002:
                continue
            print(f"draw {i} rt {draws[i]['rt']} ({' '.join(f'{x:.3f}' for x in b)}) -> "
                  f"({' '.join(f'{x:.3f}' for x in a)}) | {draws[i]['state']}")
    elif mode == "image" and len(sys.argv) >= 5:
        i, scale, s = int(sys.argv[3]), float(sys.argv[5]) if len(sys.argv) >= 6 else 1.0, 8
        pixels = bytearray()
        for y in range(GRID_H * s):
            for x in range(GRID_W * s):
                p = (y // s) * GRID_W + x // s
                for c in range(3):
                    pixels.append(round(min(max(grids[i][p * 4 + c] * scale, 0.0), 1.0) * 255))
        with open(sys.argv[4], "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (GRID_W * s, GRID_H * s) + bytes(pixels))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
