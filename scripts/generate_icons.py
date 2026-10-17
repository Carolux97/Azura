#!/usr/bin/env python3
"""
Generates all production icons (.icns, .ico, .png) for both runtime and launcher.
Uses macOS native `sips` and `iconutil` without requiring third-party libraries.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

def run(cmd):
    subprocess.run(cmd, check=True)

def resize(src, dst, width, height=None):
    if height is None:
        height = width
    run(["sips", "-z", str(height), str(width), src, "--out", dst])

def make_ico(png_files, out_ico):
    """Combines a list of PNG files into a valid modern Windows .ico file."""
    # ICO Header: 2 bytes reserved (0), 2 bytes type (1 = icon), 2 bytes count
    num_images = len(png_files)
    header = struct.pack("<HHH", 0, 1, num_images)
    
    entries = []
    data_blobs = []
    offset = 6 + (num_images * 16)
    
    for png_path in png_files:
        with open(png_path, "rb") as f:
            png_data = f.read()
        
        # Parse PNG dimensions from IHDR chunk (bytes 16..24)
        w, h = struct.unpack(">II", png_data[16:24])
        # ICO entry byte width/height (0 means 256)
        entry_w = 0 if w >= 256 else w
        entry_h = 0 if h >= 256 else h
        size = len(png_data)
        
        # ICONDIRENTRY: bWidth, bHeight, bColorCount, bReserved, wPlanes, wBitCount, dwBytesInRes, dwImageOffset
        entry = struct.pack("<BBBBHHII", entry_w, entry_h, 0, 0, 1, 32, size, offset)
        entries.append(entry)
        data_blobs.append(png_data)
        offset += size
        
    with open(out_ico, "wb") as f:
        f.write(header)
        for e in entries:
            f.write(e)
        for d in data_blobs:
            f.write(d)

def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    src_master = os.path.join(repo_root, "runtime/assets/azura-icon.png")
    
    if not os.path.exists(src_master):
        print(f"Error: master icon not found at {src_master}")
        sys.exit(1)
        
    with tempfile.TemporaryDirectory() as tmpdir:
        # 1. Generate macOS .icns
        iconset_dir = os.path.join(tmpdir, "azura.iconset")
        os.makedirs(iconset_dir, exist_ok=True)
        
        iconset_specs = [
            ("icon_16x16.png", 16),
            ("icon_16x16@2x.png", 32),
            ("icon_32x32.png", 32),
            ("icon_32x32@2x.png", 64),
            ("icon_128x128.png", 128),
            ("icon_128x128@2x.png", 256),
            ("icon_256x256.png", 256),
            ("icon_256x256@2x.png", 512),
            ("icon_512x512.png", 512),
            ("icon_512x512@2x.png", 1024),
        ]
        
        for name, size in iconset_specs:
            target = os.path.join(iconset_dir, name)
            resize(src_master, target, size)
            
        runtime_icns = os.path.join(repo_root, "runtime/assets/azura-icon.icns")
        run(["iconutil", "-c", "icns", iconset_dir, "-o", runtime_icns])
        print(f"Created: {runtime_icns}")
        
        # 2. Generate Windows .ico
        ico_sizes = [16, 24, 32, 48, 64, 128, 256]
        ico_pngs = []
        for s in ico_sizes:
            p = os.path.join(tmpdir, f"ico_{s}x{s}.png")
            resize(src_master, p, s)
            ico_pngs.append(p)
            
        runtime_ico = os.path.join(repo_root, "runtime/assets/azura-icon.ico")
        make_ico(ico_pngs, runtime_ico)
        print(f"Created: {runtime_ico}")

    print(f"Runtime icons successfully generated from {src_master}!")

if __name__ == "__main__":
    main()


