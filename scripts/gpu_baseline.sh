#!/usr/bin/env bash
# Records the per-frame performance counters of one play session and summarizes the GPU time.
#
#   scripts/gpu_baseline.sh start <label> [setting=value ...]
#                                            enable perf_log_csv in the installed azura.toml, and
#                                            the given settings for this session only
#                                            (e.g. shader_compiler=xsc)
#   (play the route, quit the game)
#   scripts/gpu_baseline.sh finish          restore azura.toml and print the summary
#   scripts/gpu_baseline.sh summary <csv>   print the summary of a CSV recorded earlier
#
# Works on Linux and macOS with the launcher's installed runtime. The CSV stays in
# <launcher data>/gpu_baseline/; the summary is what to paste on the Trello card.

set -euo pipefail

if [[ "$(uname -s)" == "Darwin" ]]; then
  DATA_DIR="${HOME}/Library/Application Support/com.gears3azura.launcher"
else
  DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/com.gears3azura.launcher"
fi
CONFIG="${DATA_DIR}/runtime/azura.toml"
OUT_DIR="${DATA_DIR}/gpu_baseline"
BACKUP="${OUT_DIR}/azura.toml.before-baseline"
CURRENT="${OUT_DIR}/current"

summary() {
  python3 - "$1" <<'EOF'
import csv, statistics, sys

# The last line can be cut short: the game exits without closing the CSV.
rows = [r for r in csv.DictReader(open(sys.argv[1]))
        if r.get("host_gpu_time_us") and None not in r.values()]
# Gameplay frames: menus and loading screens draw a few hundred draws at most.
game = [r for r in rows if int(r["host_draws"]) > 300]
if not game:
    sys.exit("No gameplay frames in the CSV (was the campaign reached?)")


def stats(values):
    values = sorted(values)
    pick = lambda q: values[min(len(values) - 1, int(len(values) * q))]
    return f"median {statistics.median(values):6.1f}  p90 {pick(0.9):6.1f}  p99 {pick(0.99):6.1f}"


print(f"{len(rows)} frames, {len(game)} gameplay frames ({len(game) / 30 / 60:.1f} min at 30 FPS)")
print("Frame time (ms):  " + stats(float(r["frame_time_us"]) / 1000 for r in game))
print("GPU time (ms):    " + stats(float(r["host_gpu_time_us"]) / 1000 for r in game))
if all(float(r["host_gpu_time_us"]) == 0 for r in game):
    print("host_gpu_time_us is 0: this GPU or driver has no timestamps on the graphics queue")
print("Per minute of gameplay (GPU ms median / p90, draws, render passes):")
for i in range(0, len(game), 1800):
    window = game[i:i + 1800]
    gpu = sorted(float(r["host_gpu_time_us"]) / 1000 for r in window)
    print(f"  {i // 1800:3d}  {statistics.median(gpu):5.1f} / {gpu[int(len(gpu) * 0.9)]:5.1f}"
          f"  {statistics.median(int(r['host_draws']) for r in window):6.0f}"
          f"  {statistics.median(int(r['host_render_passes']) for r in window):4.0f}")
# Hitches: gameplay frames above twice the median frame time, and the frames the player marked
# (F8, or the controller's Back button) with the slowest frame of the two seconds before each
# mark. A frame as long as its neighbours is not a hitch but a lower steady rate (the game holds
# some scenes at 30 FPS), and is left out. Each one gets the
# cause that took most of it: the work its draws did on the command processor thread (shader
# translation, texture loading, getting a pipeline), waiting for the GPU, or GPU time above the
# frame time. Recordings older than these columns have no hitch section.
if "draw_pipeline_us" not in rows[0]:
    sys.exit()
index = {id(r): i for i, r in enumerate(rows)}
median_us = statistics.median(float(r["frame_time_us"]) for r in game)
def steady(i):
    """Whether frame i is as long as the frames around it (within 15%)."""
    frame_us = float(rows[i]["frame_time_us"])
    around = rows[max(0, i - 3):i] + rows[i + 1:i + 4]
    return len(around) == 6 and all(abs(float(r["frame_time_us"]) - frame_us) < 0.15 * frame_us
                                    for r in around)
hitches = {id(r): r for r in game
           if float(r["frame_time_us"]) > 2 * median_us and not steady(index[id(r)])}
marks = [i for i, r in enumerate(rows) if int(r.get("markers") or 0) > 0]
for i in marks:
    before = rows[max(0, i - int(2e6 / median_us)):i + 1]
    slowest = max(before, key=lambda r: float(r["frame_time_us"]))
    hitches[id(slowest)] = slowest
causes = {
    "shader translation": "draw_shader_translation_us",
    "texture loading": "draw_texture_load_us",
    "pipeline": "draw_pipeline_us",
    "GPU wait": "host_gpu_wait_us",
}
threads = {}
threads_csv = sys.argv[1][:-len(".csv")] + ".threads.csv"
try:
    for t in csv.DictReader(open(threads_csv)):
        # The last line can be cut short, as in the frame CSV.
        if None in t.values():
            continue
        threads.setdefault(int(t["frame"]), []).append((int(t["cpu_us"]), t["name"]))
except (FileNotFoundError, KeyError, TypeError, ValueError):
    pass
print(f"Hitches (frames above {2 * median_us / 1000:.1f} ms, and {len(marks)} marked by the player):")
totals = {}
for r in sorted(hitches.values(), key=lambda r: index[id(r)]):
    frame_us = float(r["frame_time_us"])
    parts = {name: float(r[column]) for name, column in causes.items()}
    gpu_us = float(r["host_gpu_time_us"])
    if gpu_us > median_us * 1.5:
        parts["GPU time"] = gpu_us - median_us
    cause, cause_us = max(parts.items(), key=lambda item: item[1])
    if frame_us < 1.5 * median_us:
        cause = "no hitch"
    elif cause_us < (frame_us - median_us) / 3:
        cause = "other"
    totals[cause] = totals.get(cause, 0) + 1
    i = index[id(r)]
    marked = " (marked)" if any(i <= m <= i + int(2e6 / median_us) for m in marks) else ""
    detail = "  ".join(f"{name} {us / 1000:.1f}" for name, us in parts.items() if us >= 500)
    print(f"  frame {i:6d}{marked}: {frame_us / 1000:6.1f} ms  -> {cause:18s} {detail}")
    if cause == "other" and i in threads:
        busiest = sorted(threads[i], reverse=True)[:3]
        print("        busiest threads: " + ", ".join(f"{n or '?'} {us / 1000:.1f} ms" for us, n in busiest))
print("  by cause: " + ", ".join(f"{cause} {count}" for cause, count in sorted(totals.items())))
EOF
}

case "${1:-}" in
  start)
    label="${2:?usage: $0 start <label> [setting=value ...]}"
    [[ -f "${CONFIG}" ]] || { echo "No ${CONFIG}: start the game once from the launcher first" >&2; exit 1; }
    mkdir -p "${OUT_DIR}"
    [[ -f "${BACKUP}" ]] && { echo "A session is already started; run '$0 finish' first" >&2; exit 1; }
    cp "${CONFIG}" "${BACKUP}"
    csv="${OUT_DIR}/${label}_$(date +%Y%m%d-%H%M%S).csv"
    shift 2
    # Every session records with perf_marker_button: the controller's Back button (View, Create or
    # Share) marks a hitch the player notices, as F8 does, instead of reaching the game.
    set -- perf_marker_button=true "$@"
    # A TOML key may appear only once: drop the earlier values of the keys set here (the backup
    # keeps them).
    keys="perf_log_csv"
    for setting in "$@"; do
      keys="${keys}|${setting%%=*}"
    done
    grep -Ev "^[[:space:]]*(${keys})[[:space:]]*=" "${BACKUP}" > "${CONFIG}" || true
    printf '\nperf_log_csv = "%s"\n' "${csv}" >> "${CONFIG}"
    for setting in "$@"; do
      key="${setting%%=*}"
      value="${setting#*=}"
      # Numbers and booleans as they are, everything else as a string.
      if [[ "${value}" =~ ^(true|false|-?[0-9]+(\.[0-9]+)?)$ ]]; then
        printf '%s = %s\n' "${key}" "${value}" >> "${CONFIG}"
      else
        printf '%s = "%s"\n' "${key}" "${value}" >> "${CONFIG}"
      fi
      echo "For this session: ${key} = ${value}"
    done
    echo "${csv}" > "${CURRENT}"
    echo "Recording to ${csv}"
    echo "Start the game from the launcher, play the route, quit, then run: $0 finish"
    ;;
  finish)
    [[ -f "${BACKUP}" ]] || { echo "No session started" >&2; exit 1; }
    mv "${BACKUP}" "${CONFIG}"
    csv="$(cat "${CURRENT}")"
    rm -f "${CURRENT}"
    echo "azura.toml restored. CSV: ${csv}"
    summary "${csv}"
    ;;
  summary)
    summary "${2:?usage: $0 summary <csv>}"
    ;;
  *)
    sed -n '2,10p' "$0"
    exit 1
    ;;
esac
