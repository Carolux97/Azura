#!/usr/bin/env bash
# Builds the Gears3-Azura runtime on macOS (Apple Silicon) or Linux (x86_64, ARM64), from default.xex
# to the executable.
#
# Steps:
#   1. check that the SDK and the game executable are where the project expects them;
#   2. identify the executable by its SHA-256 in versions.toml, pick the manifest of that build and
#      point runtime/azura_manifest.toml at it (the generated CMake re-runs codegen with that fixed
#      name whenever an input changes, so the name must always lead to the right manifest);
#   3. apply our patches (patches/rexglue-0.10.0*.patch) to the ReXGlue SDK source tree, which the
#      runtime builds from (input changes the prebuilt SDK cannot carry), and, on macOS, build the
#      patched MoltenVK;
#   4. on a first build (no runtime/generated/rexglue.cmake), configure the SDK alone, build the
#      patched code generator and run `rexglue codegen` with it to write the generated CMake files;
#   5. configure, build the patched code generator from source, regenerate the recompiled code with
#      it when it changed (default.xex to C++ under runtime/generated/), and build the runtime.
#
# The prebuilt SDK of the platform is still used for the Vulkan headers. Every platform builds from
# the patched SDK source: the patches carry fixes the runtime needs everywhere, and the patched code
# generator emits the guest's memory fences and lets the interpreter run branch targets no function
# covers (the prebuilt one stops on them).
#
# Usage: scripts/build.sh [debug|relwithdebinfo|release]   (default: relwithdebinfo)

set -euo pipefail

readonly CONFIG="${1:-relwithdebinfo}"
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly RUNTIME_DIR="${ROOT}/runtime"
readonly XEX="${RUNTIME_DIR}/game/default.xex"
readonly REGISTRY="${ROOT}/versions.toml"
readonly SDK_SRC="${ROOT}/sdk/rexglue-0.10.0-src"
# shellcheck source=sdk_prepare.sh
source "${ROOT}/scripts/sdk_prepare.sh"
readonly PRESET="${PLATFORM}-sdk-source"

case "${CONFIG}" in
  debug|relwithdebinfo|release) ;;
  *)
    echo "Unknown configuration '${CONFIG}'. Use debug, relwithdebinfo or release." >&2
    exit 1
    ;;
esac

require_sdk

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

readonly XEX_HASH="$(sha256 "${XEX}")"
readonly MANIFEST_REL="$(manifest_for_hash "${XEX_HASH}")"
if [[ -z "${MANIFEST_REL}" ]]; then
  echo "This default.xex (SHA-256 ${XEX_HASH}) is not a supported build." >&2
  echo "Supported builds are listed in versions.toml." >&2
  exit 1
fi
readonly MANIFEST="${ROOT}/${MANIFEST_REL}"
if [[ "$(dirname "${MANIFEST}")" != "${RUNTIME_DIR}" ]]; then
  echo "Manifest ${MANIFEST_REL} must be in runtime/, where its relative paths resolve." >&2
  exit 1
fi
# Relative link, so the checkout can move without breaking it.
ln -sfn "$(basename "${MANIFEST}")" "${RUNTIME_DIR}/azura_manifest.toml"

prepare_sdk

# CMake reads CMakePresets.json from the current folder, so both steps run from runtime/.
# The runtime needs runtime/generated/rexglue.cmake, which codegen writes. Without it (a first
# build), the configure step sets up the SDK alone (see runtime/CMakeLists.txt): the patched code
# generator is built there and writes it, and the runtime is configured again.
if [[ ! -f "${RUNTIME_DIR}/generated/rexglue.cmake" ]]; then
  echo "==> Bootstrapping the generated CMake files with the patched code generator"
  (cd "${RUNTIME_DIR}" && cmake --preset "${PRESET}")
  (cd "${RUNTIME_DIR}" && cmake --build --preset "${PRESET}-${CONFIG}" --target rexglue)
  BOOTSTRAP_GENERATOR="$(ls -t $(find "${SDK_SRC}/out" -name 'rexglue*' -type f -perm -u+x) | head -n 1)"
  (cd "${RUNTIME_DIR}" && "${BOOTSTRAP_GENERATOR}" codegen azura_manifest.toml)
fi
echo "==> Configuring"
(cd "${RUNTIME_DIR}" && cmake --preset "${PRESET}")

# The recompiled code must come from the patched generator (it emits real memory fences for the
# guest's sync/lwsync; the prebuilt one drops them, which breaks the game's lock-free queues on
# ARM64). CMake re-runs codegen only when its inputs change, so the stamp is dropped whenever the
# generator is newer than the code it wrote.
echo "==> Building the code generator"
(cd "${RUNTIME_DIR}" && cmake --build --preset "${PRESET}-${CONFIG}" --target rexglue)
readonly CODEGEN_STAMP="${RUNTIME_DIR}/generated/default/codegen.build.stamp"
readonly GENERATOR="$(find "${SDK_SRC}/out" -name 'rexglue*' -type f -perm -u+x -newer "${CODEGEN_STAMP}" 2>/dev/null | head -n 1)"
if [[ ! -f "${CODEGEN_STAMP}" || -n "${GENERATOR}" ]]; then
  echo "==> Code generator changed: regenerating the recompiled code"
  rm -f "${CODEGEN_STAMP}"
fi

echo "==> Building (${CONFIG})"
(cd "${RUNTIME_DIR}" && cmake --build --preset "${PRESET}-${CONFIG}")

CONFIG_DIR="RelWithDebInfo"
case "${CONFIG}" in
  debug) CONFIG_DIR="Debug" ;;
  release) CONFIG_DIR="Release" ;;
esac

# On macOS, verify the staged MoltenVK is not the unpatched SDK original.
if [[ "${PLATFORM}" == mac-* ]]; then
  readonly STAGED_MVK="${RUNTIME_DIR}/out/build/${PRESET}/${CONFIG_DIR}/vulkan/lib/libMoltenVK.dylib"
  readonly UNPATCHED_MVK_HASH="$(awk '/^\[moltenvk\]/{s=1;next} /^\[/{s=0} s && /^unpatched_sha256[[:space:]]*=/{v=$0;sub(/^[^"]*"/,"",v);sub(/".*$/,"",v);print v;exit}' "${REGISTRY}")"
  readonly STAGED_MVK_HASH="$(sha256 "${STAGED_MVK}")"
  if [[ "${STAGED_MVK_HASH}" == "${UNPATCHED_MVK_HASH}" ]]; then
    echo "Error: staged ${STAGED_MVK} matches unpatched MoltenVK (${UNPATCHED_MVK_HASH})!" >&2
    exit 1
  fi
fi

echo "==> Done. Output: ${RUNTIME_DIR}/out/build/${PRESET}/${CONFIG_DIR}/"

