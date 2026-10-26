#!/usr/bin/env python3
"""Summarizes the runs of scripts/hle/compare_modes.sh, per graphics path and scene.

Each run's perf.csv has one row per guest frame. Frames are grouped into scenes by what the game
draws (guest draws and resolves per frame), since runs reach each scene at a slightly different
time: the title screen, the main menu and the first playable moment of the campaign. A run that
never reaches a scene (a scripted press arrived before the game took input) adds nothing to it. For each scene and
mode it prints the median over all rounds of: frame time, host GPU time, guest draws, resolves,
render passes, render target transfers (made and skipped); and over the whole run: pipeline
creations, shader translations and frames over 50 ms.

With --images it also compares the captures. Runs drift by a few seconds (loading takes a
different time each run), so each capture of a run is compared with the captures of every gfx =
pm4 run at the same second and the ones 4 seconds before and after, keeping the closest: for pm4
runs against the other pm4 runs, that is the noise floor (film grain, animation, timing); for hle
runs, the distance to pm4. Pixels are sampled one in four on each axis.

Usage: scripts/hle/summarize_compare.py [OUT_DIR] [--images]   (default captures/compare)
"""

import csv
import glob
import os
import re
import statistics
import sys

# Scenes, by the guest draws and resolves of their frames (the steady frames of each).
SCENES = [
    ("title screen", (350, 362), 30),
    ("main menu", (385, 402), 30),
    ("campaign start", (700, 740), 38),
]
PER_FRAME = [
    ("frame ms", "frame_time_us", 1e-3),
    ("GPU ms", "host_gpu_time_us", 1e-3),
    ("draws", "draw_calls", 1),
    ("resolves", "resolves", 1),
    ("render passes", "host_render_passes", 1),
    ("transfers", "render_target_transfers", 1),
    ("transfers skipped", "render_target_transfers_skipped", 1),
    ("transfers cut out", "render_target_transfers_cut_out", 1),
    ("transfer tiles", "render_target_transfer_tiles", 1),
    ("pass ends: transfer", "pass_ends_transfer", 1),
    ("pass ends: resolve", "pass_ends_resolve", 1),
    ("draws in transfer passes", "draws_in_transfer_passes", 1),
    ("clears as load actions", "clears_as_load_actions", 1),
    ("resolves to textures", "resolves_to_textures", 1),
    ("resolves from render targets", "resolves_from_render_targets", 1),
    ("transfer audit: read", "transfer_audit_read", 1),
    ("transfer audit: written", "transfer_audit_written", 1),
    ("transfer audit: untouched", "transfer_audit_untouched", 1),
    ("transfers reinterpreting", "transfers_reinterpreting", 1),
    ("passes resumed after resolve", "render_passes_resumed_after_resolve", 1),
    ("  of them after a native one", "render_passes_resumed_after_native_resolve", 1),
    ("pass ends: texture load", "pass_ends_texture_load", 1),
    ("host dispatches", "host_dispatches", 1),
]
SPIKE_US = 50000
# Windows of the captures (one every 4 seconds), in seconds since start, for the scripted
# controller of compare_modes.sh. Captures from CAMPAIGN_FROM_S on only count for runs that
# reached the campaign.
IMAGE_SCENES = [
    ("title screen", 40, 56),
    ("main menu", 76, 92),
    ("campaign cutscene", 140, 200),
    ("campaign start", 212, 240),
]
CAMPAIGN_FROM_S = 120


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


CAPTURE_STEP_S = 4
SAMPLE_STEP = 4
TOLERANCE = 16


def read_ppm_sampled(path):
    """The pixels of a capture, one in SAMPLE_STEP on each axis, or None if incomplete."""
    with open(path, "rb") as f:
        data = f.read()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]
    if len(pixels) < width * height * 3:
        return None
    sampled = bytearray()
    for y in range(0, height, SAMPLE_STEP):
        row = pixels[y * width * 3:(y + 1) * width * 3]
        for x in range(0, width, SAMPLE_STEP):
            sampled += row[x * 3:x * 3 + 3]
    return bytes(sampled)


def distance(a, b):
    """Share of sampled pixels whose largest channel difference exceeds TOLERANCE, in percent."""
    over = 0
    for i in range(0, min(len(a), len(b)), 3):
        if max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2])) > TOLERANCE:
            over += 1
    return 100.0 * over / (len(a) // 3)


def compare_images(out_dir, scenes, reached_campaign):
    runs = {}
    for frames in sorted(glob.glob(os.path.join(out_dir, "*_*", "frames"))):
        name = os.path.basename(os.path.dirname(frames))
        captures = {}
        campaign_ok = reached_campaign.get(name, False)
        for path in glob.glob(os.path.join(frames, "frame_*.ppm")):
            second = int(re.search(r"frame_(\d+)", path).group(1))
            if second >= CAMPAIGN_FROM_S and not campaign_ok:
                continue  # this run stayed in the menus
            pixels = read_ppm_sampled(path)
            if pixels:
                captures[second] = pixels
        runs[name] = captures
    references = [n for n in runs if n.startswith("pm4_")]
    print(f"\nimages: % of pixels differing by more than {TOLERANCE} from the closest pm4 "
          "capture (median / worst over the captures of the scene)")
    modes = sorted({n.split("_")[0] for n in runs})
    print(f"  {'':22s}" + "".join(f"{m:>16s}" for m in modes))
    for scene, start, end in scenes:
        cells = []
        for mode in modes:
            values = []
            for name, captures in runs.items():
                if not name.startswith(mode + "_"):
                    continue
                for second, pixels in captures.items():
                    if not start <= second < end:
                        continue
                    best = None
                    for reference in references:
                        if reference == name:
                            continue
                        for t in (second - CAPTURE_STEP_S, second, second + CAPTURE_STEP_S):
                            other = runs[reference].get(t)
                            if other:
                                d = distance(pixels, other)
                                best = d if best is None else min(best, d)
                    if best is not None:
                        values.append(best)
            cells.append(f"{statistics.median(values):7.1f} / {max(values):5.1f}" if values
                         else f"{'n/a':>16s}")
        print(f"  {scene:22s}" + "".join(f"{c:>16s}" for c in cells))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out_dir = args[0] if args else "captures/compare"
    runs = {}
    reached_campaign = {}
    campaign_draws, _ = SCENES[2][1], SCENES[2][2]
    for path in sorted(glob.glob(os.path.join(out_dir, "*_*", "perf.csv"))):
        name = os.path.basename(os.path.dirname(path))
        rows = load(path)
        runs.setdefault(name.split("_")[0], []).append(rows)
        reached_campaign[name] = any(campaign_draws[0] <= r["draw_calls"] <= campaign_draws[1]
                                     for r in rows)
    if not runs:
        sys.exit(f"No runs in {out_dir}")
    modes = sorted(runs)
    print(f"Rounds: " + ", ".join(f"{m} {len(runs[m])}" for m in modes))
    for scene, (draws_min, draws_max), resolves in SCENES:
        def in_scene(r):
            return draws_min <= r["draw_calls"] <= draws_max and r["resolves"] == resolves
        reached = {m: sum(1 for rows in runs[m] if any(in_scene(r) for r in rows)) for m in modes}
        print(f"\n{scene} (runs that reached it: " +
              ", ".join(f"{m} {reached[m]}" for m in modes) + ")")
        print(f"  {'':22s}" + "".join(f"{m:>12s}" for m in modes))
        for label, column, scale in PER_FRAME:
            cells = []
            for mode in modes:
                values = [r[column] * scale for rows in runs[mode] for r in rows
                          if in_scene(r) and column in r]
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
    if "--images" in sys.argv:
        compare_images(out_dir, IMAGE_SCENES, reached_campaign)


if __name__ == "__main__":
    main()
