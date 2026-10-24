#!/usr/bin/env bash
# Starts the installed runtime several times in a row and checks that each start shows a picture.
#
#   scripts/start_bench.sh [runs] [seconds]
#
# Each run starts the game as the launcher does (same runtime, settings and shader cache, but an
# empty saves folder outside ~/Documents, so the run never waits on macOS's Documents prompt),
# captures the screen every 2 seconds until one shows a picture or `seconds` (default 15) have
# passed, and stops the game. A run with no picture in that time counts as a black start, and its
# captures and log are kept; every other run keeps the capture that showed the picture. macOS
# only: it uses screencapture and the game in full screen.

set -euo pipefail

RUNS="${1:-10}"
SECONDS_PER_RUN="${2:-15}"
# Seconds between captures.
CAPTURE_EVERY=2
DATA_DIR="${HOME}/Library/Application Support/com.gears3azura.launcher"
RUNTIME_DIR="${DATA_DIR}/runtime"
OUT_DIR="${DATA_DIR}/start_bench/$(date +%Y%m%d-%H%M%S)"
SAVES_DIR="${OUT_DIR}/saves"
# A capture shows a picture when its brightest pixel, after scaling the capture down, reaches this
# (0 to 255).
BLACK_THRESHOLD=16

[[ -x "${RUNTIME_DIR}/azura" ]] || { echo "No runtime in ${RUNTIME_DIR}: install the game first" >&2; exit 1; }
if pgrep -x azura >/dev/null; then
  echo "The game is running: close it first" >&2
  exit 1
fi
mkdir -p "${SAVES_DIR}"

# Brightest and mean value (0 to 255) of a capture, scaled down to 64 pixels wide.
brightness() {
  local bmp="${OUT_DIR}/tmp.bmp"
  sips -s format bmp -Z 64 "$1" --out "${bmp}" >/dev/null
  python3 -I - "${bmp}" <<'EOF'
import struct, sys
data = open(sys.argv[1], "rb").read()
offset = struct.unpack_from("<I", data, 10)[0]
width, height = struct.unpack_from("<ii", data, 18)
bits = struct.unpack_from("<H", data, 28)[0]
step = bits // 8
row = (width * step + 3) & ~3
values = []
for y in range(abs(height)):
    for x in range(width):
        b, g, r = data[offset + y * row + x * step:offset + y * row + x * step + 3]
        values.append((r * 299 + g * 587 + b * 114) // 1000)
print(max(values), sum(values) // len(values))
EOF
}

# A 40-character bar with the elapsed and estimated time left.
progress() {
  local done="$1" started="$2"
  local now elapsed left filled
  now="$(date +%s)"
  elapsed=$((now - started))
  if (( done > 0 )); then
    left=$((elapsed * (RUNS - done) / done))
  else
    left=$(((SECONDS_PER_RUN + 10) * RUNS))
  fi
  filled=$((40 * done / RUNS))
  printf '\r[%-40s] %d/%d  elapsed %dm%02ds  left ~%dm%02ds ' \
    "$(printf '%*s' "${filled}" '' | tr ' ' '#')" "${done}" "${RUNS}" \
    $((elapsed / 60)) $((elapsed % 60)) $((left / 60)) $((left % 60))
}

echo "Start bench: ${RUNS} runs of ${SECONDS_PER_RUN} s. Results in ${OUT_DIR}"
echo "Do not use the computer while it runs: the game takes the screen."
black=0
started="$(date +%s)"
progress 0 "${started}"
for run in $(seq 1 "${RUNS}"); do
  (
    cd "${RUNTIME_DIR}"
    exec ./azura "--user_data_root=${SAVES_DIR}" "--cache_root=${DATA_DIR}/cache" >/dev/null 2>&1
  ) &
  game=$!
  results=()
  shown=""
  for ((at = CAPTURE_EVERY; at <= SECONDS_PER_RUN; at += CAPTURE_EVERY)); do
    sleep "${CAPTURE_EVERY}"
    capture="${OUT_DIR}/run${run}_${at}s.png"
    screencapture -x -D 1 "${capture}"
    result="$(brightness "${capture}")"
    results+=("${at}s ${result}")
    if (( ${result%% *} >= BLACK_THRESHOLD )); then
      shown="${capture}"
      break
    fi
  done
  kill "${game}" 2>/dev/null || true
  for _ in $(seq 1 20); do
    kill -0 "${game}" 2>/dev/null || break
    sleep 0.5
  done
  kill -9 "${game}" 2>/dev/null || true
  wait "${game}" 2>/dev/null || true

  log="$(ls -t "${RUNTIME_DIR}/logs"/azura_*.log 2>/dev/null | head -1)"
  if [[ -z "${shown}" ]]; then
    black=$((black + 1))
    [[ -n "${log}" ]] && cp "${log}" "${OUT_DIR}/run${run}.log"
    verdict="BLACK"
  else
    # The capture with the picture stays as a record of what the start showed.
    find "${OUT_DIR}" -name "run${run}_*.png" ! -path "${shown}" -delete
    verdict="picture"
  fi
  printf 'run %2d: %-7s (time, brightest, mean per capture: %s) log %s\n' "${run}" "${verdict}" \
    "$(IFS=,; echo "${results[*]}")" "$(basename "${log:-none}")" >> "${OUT_DIR}/results.txt"
  progress "${run}" "${started}"
done
rm -f "${OUT_DIR}/tmp.bmp"
rm -rf "${SAVES_DIR}"
echo
cat "${OUT_DIR}/results.txt"
echo "Black starts: ${black} of ${RUNS}"
