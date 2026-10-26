#!/usr/bin/env bash
# Compares the graphics paths (gfx = pm4 and gfx = hle) on the same scripted runs, alternating the
# order every round so heat and background load affect them alike (docs/hle-graphics/03-results.md).
# VARIANTS lists what to compare as "name:runtime options" separated by "|" (names without "_";
# default "pm4:--gfx=pm4|hle:--gfx=hle"), for example to compare an HLE option on and off.
#
# Each run starts the built runtime with a fresh saves folder outside ~/Documents, the scripted
# controller (AZURA_SCRIPTED_PAD) that reaches the campaign start, periodic captures
# (AZURA_CAPTURE_DIR) and the per-frame counters (perf_log_csv), and stops it after SECONDS. The
# output is the game's and stays local: OUT_DIR defaults to captures/compare (ignored by git).
#
# Usage: scripts/hle/compare_modes.sh [ROUNDS] [SECONDS] [OUT_DIR] [extra runtime options...]
#   (default: 3 rounds of 240 seconds, Metal; pass --gpu_backend=vulkan for Vulkan)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROUNDS="${1:-3}"
SECONDS_PER_RUN="${2:-240}"
OUT_DIR="${3:-${ROOT}/captures/compare}"
# Absolute: the game runs from the runtime folder.
mkdir -p "${OUT_DIR}"
OUT_DIR="$(cd "${OUT_DIR}" && pwd)"
shift $(( $# < 3 ? $# : 3 ))
RUNTIME_DIR="${ROOT}/runtime/out/build/mac-arm64-sdk-source/RelWithDebInfo"
GAME_DIR="$(awk -F'"' '/^game_data_root/{print $2; exit}' \
  "${HOME}/Library/Application Support/com.gears3azura.launcher/runtime/azura.toml")"
# Start (twice, in case the title takes input late), Campaign, Standard, New, then accept the screens
# up to the chapter. Timed for a run that compiles its shaders from scratch.
PAD_SCRIPT="55:start,59:start,68:a,78:a,86:down,88:a,96:a,103:a,110:a,117:a,124:a"

[[ -x "${RUNTIME_DIR}/azura" ]] || { echo "No runtime in ${RUNTIME_DIR}: build it first" >&2; exit 1; }
[[ -d "${GAME_DIR}" ]] || { echo "No game folder (${GAME_DIR})" >&2; exit 1; }
if pgrep -x azura >/dev/null; then
  echo "The game is running: close it first" >&2
  exit 1
fi
mkdir -p "${OUT_DIR}"

VARIANTS="${VARIANTS:-pm4:--gfx=pm4|hle:--gfx=hle}"
IFS='|' read -r -a VARIANT_LIST <<< "${VARIANTS}"

run() {
  local mode="$1" round="$2" options="$3"
  shift 3
  local tag="${mode}_${round}"
  local work="${OUT_DIR}/${tag}"
  # One game at a time: two would share the GPU and spoil both measurements.
  if pgrep -x azura >/dev/null; then
    echo "Another game is running: stopping the comparison" >&2
    exit 1
  fi
  rm -rf "${work}"
  mkdir -p "${work}/saves" "${work}/cache"
  echo "==> round ${round}, ${mode} (${options})"
  # exec, so the background job's PID is the game's own and stopping it stops the game.
  (cd "${RUNTIME_DIR}" && AZURA_SCRIPTED_PAD="${PAD_SCRIPT}" AZURA_CAPTURE_DIR="${work}/frames" \
    AZURA_CAPTURE_EVERY=4 exec ./azura "--game_data_root=${GAME_DIR}" "--user_data_root=${work}/saves" \
    "--cache_root=${work}/cache" "--log_file=${work}/run.log" --log_max_file_size_mb=4000 \
    "--perf_log_csv=${work}/perf.csv" ${options} "$@" >/dev/null 2>&1) &
  local pid=$!
  local end=$((SECONDS + SECONDS_PER_RUN))
  while kill -0 "${pid}" 2>/dev/null && (( SECONDS < end )); do
    sleep 1
  done
  if kill -0 "${pid}" 2>/dev/null; then
    kill "${pid}"
    wait "${pid}" 2>/dev/null || true
    # The game must be gone before the next run starts.
    while pgrep -x azura >/dev/null; do
      sleep 1
    done
  else
    echo "    the game exited before ${SECONDS_PER_RUN} s" >&2
  fi
  # Each run compiles its own shaders (fresh cache), as a first run would.
  rm -rf "${work}/saves" "${work}/cache"
}

for (( round = 1; round <= ROUNDS; ++round )); do
  # Alternate the order, so no variant always runs on a cooler machine.
  order=("${VARIANT_LIST[@]}")
  if (( round % 2 == 0 )); then
    order=()
    for (( i = ${#VARIANT_LIST[@]} - 1; i >= 0; --i )); do
      order+=("${VARIANT_LIST[i]}")
    done
  fi
  for variant in "${order[@]}"; do
    run "${variant%%:*}" "${round}" "${variant#*:}" "$@"
  done
done
echo "==> Done: ${OUT_DIR}"
