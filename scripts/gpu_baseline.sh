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
