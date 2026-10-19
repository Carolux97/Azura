#!/usr/bin/env python3
"""Summarises the CPU time each thread used per frame, from the runtime's per-thread CSV log.

With `perf_log_csv` set and `perf_log_threads = true`, the runtime writes two files: the frame CSV
(one row per frame: frame time, FPS, draw calls...) and, beside it, `<name>.threads.csv`, with one
row per thread and frame holding the CPU time that thread used during the frame. A thread with no
row in a frame used no CPU in it.

This script takes a range of frames (by default the last 560, the usual standing-still sample at
the campaign start) and prints, for every thread that used CPU in it, the median, mean and 90th
percentile of its CPU time per frame, next to the median frame time. A thread whose CPU time per
frame is close to the frame time is busy all frame long and is the likely limit; it can also be
busy-waiting, which counts as CPU time too.

Usage: thread_cpu.py <frame CSV> [--first N] [--last N] [--top N]
  --first, --last  frame range, 0-based rows of the frame CSV, both included
                   (default: the last 560 frames)
  --top            how many threads to list, busiest first (default: 15)
"""

import argparse
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT_FRAMES = 560


def percentile(values, fraction):
    """Value below which the given fraction of the sorted values falls (nearest rank)."""
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * fraction))]


def main():
    parser = argparse.ArgumentParser(description="Per-thread CPU time per frame.")
    parser.add_argument("frame_csv", type=Path)
    parser.add_argument("--first", type=int)
    parser.add_argument("--last", type=int)
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args()

    threads_csv = args.frame_csv.with_suffix(".threads.csv")
    if not threads_csv.exists():
        sys.exit(f"{threads_csv} not found: run the game with perf_log_threads = true")

    with args.frame_csv.open(newline="") as f:
        frame_times_ms = [int(row["frame_time_us"]) / 1000 for row in csv.DictReader(f)]
    last = args.last if args.last is not None else len(frame_times_ms) - 1
    first = args.first if args.first is not None else max(0, last - DEFAULT_FRAMES + 1)
    frames = range(first, last + 1)
    if not frames:
        sys.exit("empty frame range")

    # CPU time per frame of every thread, keyed by (id, name): ids are unique while a thread lives,
    # names tell the threads apart for the reader and can repeat (XThread workers, for example).
    cpu_ms = defaultdict(lambda: defaultdict(float))
    with threads_csv.open(newline="") as f:
        for row in csv.DictReader(f):
            # The runtime flushes the log every 60 frames, so a game that is closed can leave the
            # last row cut short.
            if row["cpu_us"] is None:
                continue
            frame = int(row["frame"])
            if first <= frame <= last:
                cpu_ms[(row["thread_id"], row["name"])][frame] += int(row["cpu_us"]) / 1000

    frame_median = statistics.median(frame_times_ms[first : last + 1])
    print(f"Frames {first}-{last} ({len(frames)}), median frame time {frame_median:.1f} ms "
          f"({1000 / frame_median:.1f} FPS)\n")
    print(f"{'thread':40} {'median':>7} {'mean':>7} {'p90':>7} {'of frame':>9}")

    rows = []
    for (thread_id, name), per_frame in cpu_ms.items():
        # Frames without a row count as 0 ms for that thread.
        values = [per_frame.get(frame, 0.0) for frame in frames]
        rows.append((statistics.mean(values), name or f"(unnamed {thread_id})", values))
    for mean, name, values in sorted(rows, reverse=True)[: args.top]:
        median = statistics.median(values)
        print(f"{name[:40]:40} {median:7.1f} {mean:7.1f} {percentile(values, 0.9):7.1f} "
              f"{median / frame_median:8.0%}")


if __name__ == "__main__":
    main()
