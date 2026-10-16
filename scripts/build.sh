#!/usr/bin/env bash
# Builds the Gears3-Azura runtime on macOS (Apple Silicon), from default.xex to the executable.
#
# Steps:
#   1. check that the SDK and the game executable are where the project expects them;
#   2. identify the executable by its SHA-256 in versions.toml and pick the manifest of that build;
#   3. run `rexglue codegen`, which translates default.xex into C++ under runtime/generated/
#      (it skips the work when the inputs have not changed since the last run);
#   4. configure and build with the CMake preset.
#
# Usage: scripts/build.sh [debug|relwithdebinfo|release]   (default: relwithdebinfo)

set -euo pipefail

readonly CONFIG="${1:-relwithdebinfo}"
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly RUNTIME_DIR="${ROOT}/runtime"
readonly SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-mac-arm64/mac-arm64"
readonly REXGLUE="${SDK_DIR}/bin/rexglue"
readonly XEX="${RUNTIME_DIR}/game/default.xex"
readonly REGISTRY="${ROOT}/versions.toml"

case "${CONFIG}" in
  debug|relwithdebinfo|release) ;;
  *)
    echo "Unknown configuration '${CONFIG}'. Use debug, relwithdebinfo or release." >&2
    exit 1
    ;;
esac

if [[ ! -x "${REXGLUE}" ]]; then
  echo "ReXGlue SDK not found at ${SDK_DIR}. See the README for how to install it." >&2
  exit 1
fi

if [[ ! -f "${XEX}" ]]; then
  echo "Game executable not found at ${XEX}." >&2
  echo "Copy default.xex from your own Gears of War 3 dump into runtime/game/." >&2
  exit 1
fi

# Prints the manifest path of the [[versions]] entry whose xex_sha256 equals $1, or nothing.
# versions.toml is our own file with one `key = "value"` per line, which this reader relies on.
manifest_for_hash() {
  awk -v hash="$1" '
    /^\[\[versions\]\]/ { matched = 0; manifest = "" }
    /^xex_sha256[[:space:]]*=/ { matched = (index(tolower($0), "\"" tolower(hash) "\"") > 0) }
    /^manifest[[:space:]]*=/ { manifest = $0; sub(/^[^"]*"/, "", manifest); sub(/".*$/, "", manifest) }
    matched && manifest != "" { print manifest; exit }
  ' "${REGISTRY}"
}

readonly XEX_HASH="$(shasum -a 256 "${XEX}" | cut -d ' ' -f 1)"
readonly MANIFEST_REL="$(manifest_for_hash "${XEX_HASH}")"
if [[ -z "${MANIFEST_REL}" ]]; then
  echo "This default.xex (SHA-256 ${XEX_HASH}) is not a supported build." >&2
  echo "Supported builds are listed in versions.toml." >&2
  exit 1
fi
readonly MANIFEST="${ROOT}/${MANIFEST_REL}"

echo "==> Recompiling default.xex with ${MANIFEST_REL}"
(cd "${RUNTIME_DIR}" && "${REXGLUE}" codegen "${MANIFEST}")

echo "==> Configuring"
cmake --preset mac-arm64 -S "${RUNTIME_DIR}"

echo "==> Building (${CONFIG})"
cmake --build --preset "mac-arm64-${CONFIG}" --target azura

echo "==> Done. Output: ${RUNTIME_DIR}/out/build/mac-arm64/"
