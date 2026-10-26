#!/usr/bin/env python3
"""Compares the frame captures of two runs (runtime/src/hle_gfx/frame_capture.cpp).

For every capture name present in both folders, prints the mean absolute difference per channel
(0-255) and the share of pixels whose largest channel difference exceeds the tolerance, and with
--png writes side-by-side previews (A left, B right, scaled down) for looking at them. Captures
and previews are the game's images: keep them local (captures/ is ignored by git).

Captures from two runs are taken at the same second since start, not the same frame, so animated
scenes always differ somewhat; compare still or slow scenes, or read the share of pixels over the
tolerance together with the previews.

Usage: scripts/hle/frame_diff.py DIR_A DIR_B [--tolerance 16] [--png OUT_DIR] [--scale 4]
"""

import argparse
import os
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    if parts[0] != b"P6":
        raise ValueError(f"{path}: not a binary PPM")
    width, height = int(parts[1]), int(parts[2])
    pixels = parts[4][: width * height * 3]
    return width, height, pixels


def write_png(path, width, height, rgb_rows):
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + row for row in rgb_rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def downscale_rows(width, height, pixels, scale):
    rows = []
    for y in range(0, height - scale + 1, scale):
        start = y * width * 3
        row = pixels[start:start + width * 3]
        rows.append(bytes(row[x * 3 + c] for x in range(0, width - scale + 1, scale) for c in range(3)))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dir_a")
    parser.add_argument("dir_b")
    parser.add_argument("--tolerance", type=int, default=16)
    parser.add_argument("--png")
    parser.add_argument("--scale", type=int, default=4)
    args = parser.parse_args()

    names = sorted(set(os.listdir(args.dir_a)) & set(os.listdir(args.dir_b)))
    names = [n for n in names if n.endswith(".ppm")]
    if not names:
        sys.exit("No captures with the same name in both folders")
    if args.png:
        os.makedirs(args.png, exist_ok=True)
    print(f"{'capture':16s} {'mean abs diff':>14s} {'pixels over tol':>16s}")
    for name in names:
        wa, ha, a = read_ppm(os.path.join(args.dir_a, name))
        wb, hb, b = read_ppm(os.path.join(args.dir_b, name))
        if (wa, ha) != (wb, hb):
            print(f"{name:16s} sizes differ: {wa}x{ha} and {wb}x{hb}")
            continue
        total = 0
        over = 0
        step = 3
        for i in range(0, len(a), step):
            d0 = abs(a[i] - b[i])
            d1 = abs(a[i + 1] - b[i + 1])
            d2 = abs(a[i + 2] - b[i + 2])
            total += d0 + d1 + d2
            if max(d0, d1, d2) > args.tolerance:
                over += 1
        pixels = wa * ha
        print(f"{name:16s} {total / (pixels * 3):14.2f} {100.0 * over / pixels:15.2f}%")
        if args.png:
            rows_a = downscale_rows(wa, ha, a, args.scale)
            rows_b = downscale_rows(wb, hb, b, args.scale)
            rows = [ra + rb for ra, rb in zip(rows_a, rows_b)]
            write_png(os.path.join(args.png, name.replace(".ppm", ".png")),
                      len(rows_a[0]) // 3 * 2, len(rows), rows)


if __name__ == "__main__":
    main()
