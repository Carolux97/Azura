#!/usr/bin/env python3
"""Summarizes the runs of scripts/hle/compare_modes.sh, per graphics path and scene.

Each run's perf.csv has one row per guest frame. The rows are placed in time by adding up
frame_time_us, and grouped into the scenes the scripted controller reaches at fixed times: the
title screen, the main menu and the first playable moment of the campaign. For each scene and
mode it prints the median over all rounds of: frame time, host GPU time, guest draws, resolves,
render passes, render target transfers (made and skipped); and over the whole run: pipeline
creations, shader translations and frames over 50 ms.

Usage: scripts/hle/summarize_compare.py [OUT_DIR]   (default captures/compare)
"""

import csv
import glob
import os
import statistics
import sys

# Scenes, in seconds since start, for the scripted controller of compare_modes.sh.
SCENES = [
    ("title screen", 36, 44),
    ("main menu", 62, 72),
    ("campaign start", 175, 198),
]
PER_FRAME = [
    ("frame ms", "frame_time_us", 1e-3),
    ("GPU ms", "host_gpu_time_us", 1e-3),
    ("draws", "draw_calls", 1),
    ("resolves", "resolves", 1),
    ("render passes", "host_render_passes", 1),
    ("transfers", "render_target_transfers", 1),
    ("transfers skipped", "render_target_transfers_skipped", 1),
    ("pass ends: transfer", "pass_ends_transfer", 1),
    ("pass ends: resolve", "pass_ends_resolve", 1),
]
SPIKE_US = 50000


def load(path):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            if None in row.values() or "" in row.values():
                continue  # the last row of a run stopped mid-write
            rows.append({k: float(v) for k, v in row.items()})
    t = 0.0
    for row in rows:
        t += row["frame_time_us"] * 1e-6
        row["t"] = t
    return rows


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "captures/compare"
    runs = {}
    for path in sorted(glob.glob(os.path.join(out_dir, "*_*", "perf.csv"))):
        mode = os.path.basename(os.path.dirname(path)).split("_")[0]
        runs.setdefault(mode, []).append(load(path))
    if not runs:
        sys.exit(f"No runs in {out_dir}")
    modes = sorted(runs)
    print(f"Rounds: " + ", ".join(f"{m} {len(runs[m])}" for m in modes))
    for scene, start, end in SCENES:
        print(f"\n{scene} ({start}-{end} s)")
        print(f"  {'':22s}" + "".join(f"{m:>12s}" for m in modes))
        for label, column, scale in PER_FRAME:
            cells = []
            for mode in modes:
                values = [r[column] * scale for rows in runs[mode] for r in rows
                          if start <= r["t"] < end and column in r]
                cells.append(f"{statistics.median(values):12.1f}" if values else f"{'n/a':>12s}")
            print(f"  {label:22s}" + "".join(cells))
    print("\nwhole run (mean per run)")
    for label, column in (("pipeline creations", "pipeline_creations"),
                          ("shader translations", "shader_translations")):
        print(f"  {label:22s}" + "".join(
            f"{statistics.mean(sum(r[column] for r in rows) for rows in runs[m]):12.1f}"
            for m in modes))
    print(f"  {'frames over 50 ms':22s}" + "".join(
        f"{statistics.mean(sum(1 for r in rows if r['frame_time_us'] > SPIKE_US) for rows in runs[m]):12.1f}"
        for m in modes))


if __name__ == "__main__":
    main()
