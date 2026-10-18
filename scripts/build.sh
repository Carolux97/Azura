#!/usr/bin/env bash
# Builds the Gears3-Azura runtime on Linux or macOS, from default.xex to the executable.

set -euo pipefail

readonly CONFIG="${1:-relwithdebinfo}"
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly RUNTIME_DIR="${ROOT}/runtime"
readonly XEX="${RUNTIME_DIR}/game/default.xex"
readonly REGISTRY="${ROOT}/versions.toml"

export PKG_CONFIG_PATH="${ROOT}/sdk/compat/pkgconfig:${PKG_CONFIG_PATH:-}"

case "$(uname -s):$(uname -m)" in
  Darwin:arm64)
    readonly PLATFORM="mac-arm64"
    readonly SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-mac-arm64/mac-arm64"
    readonly SDK_SRC="${ROOT}/sdk/rexglue-0.10.0-src"
    readonly PRESET="mac-arm64-sdk-source"
    ;;
  Linux:x86_64|Linux:amd64)
    readonly PLATFORM="linux-amd64"
    readonly SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-linux-amd64/linux-amd64"
    readonly SDK_SRC="${ROOT}/sdk/rexglue-0.10.0-src"
    readonly PRESET="linux-amd64-sdk-source"
    ;;
  Linux:aarch64|Linux:arm64)
    readonly PLATFORM="linux-arm64"
    readonly SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-linux-arm64/linux-arm64"
    readonly SDK_SRC="${ROOT}/sdk/rexglue-0.10.0-src"
    readonly PRESET="linux-arm64"
    ;;
  *)
    echo "Unsupported build platform: $(uname -s) $(uname -m)." >&2
    exit 1
    ;;
esac

readonly REXGLUE="${SDK_DIR}/bin/rexglue"

case "${CONFIG}" in
  debug|relwithdebinfo|release) ;;
  *)
    echo "Unknown configuration '${CONFIG}'. Use debug, relwithdebinfo or release." >&2
    exit 1
    ;;
esac

if [[ ! -x "${REXGLUE}" ]]; then
  echo "ReXGlue SDK not found at ${SDK_DIR}. Check SDK folder setup." >&2
  exit 1
fi

if [[ ! -f "${XEX}" ]]; then
  echo "Game executable not found at ${XEX}." >&2
  echo "Copy default.xex from your own Gears of War 3 dump into runtime/game/." >&2
  exit 1
fi

# Prints the manifest path of the [[versions]] entry whose xex_sha256 equals $1, or nothing.
manifest_for_hash() {
  awk -v hash="$1" '
    /^\[\[versions\]\]/ { matched = 0; manifest = "" }
    /^xex_sha256[[:space:]]*=/ { matched = (index(tolower($0), "\"" tolower(hash) "\"") > 0) }
    /^manifest[[:space:]]*=/ { manifest = $0; sub(/^[^"]*"/, "", manifest); sub(/".*$/, "", manifest) }
    matched && manifest != "" { print manifest; exit }
  ' "${REGISTRY}"
}

if command -v sha256sum >/dev/null 2>&1; then
  readonly XEX_HASH="$(sha256sum "${XEX}" | cut -d ' ' -f 1)"
else
  readonly XEX_HASH="$(shasum -a 256 "${XEX}" | cut -d ' ' -f 1)"
fi

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

ln -sf "$(basename "${MANIFEST_REL}")" "${RUNTIME_DIR}/azura_manifest.toml"

# The CMake project needs runtime/generated/rexglue.cmake, which codegen writes.
if [[ ! -f "${RUNTIME_DIR}/generated/rexglue.cmake" ]]; then
  echo "==> Bootstrapping the generated CMake files with the prebuilt code generator"
  (cd "${RUNTIME_DIR}" && "${REXGLUE}" codegen azura_manifest.toml)
fi

# Apply patches to SDK source if present (e.g. on macOS)
apply_patch() {
  local dir="$1" patch="$2"
  if git -C "${dir}" apply --reverse --check "${patch}" 2>/dev/null; then
    return
  fi
  if ! git -C "${dir}" apply --check "${patch}" 2>/dev/null; then
    echo "Cannot apply $(basename "${patch}") to ${dir}: the tree has conflicting changes." >&2
    exit 1
  fi
  echo "==> Applying $(basename "${patch}")"
  git -C "${dir}" apply "${patch}"
}

if [[ -d "${SDK_SRC}/.git" || -f "${SDK_SRC}/.git" ]]; then
  apply_patch "${SDK_SRC}" "${ROOT}/patches/rexglue-0.10.0.patch"
  apply_patch "${SDK_SRC}/thirdparty/utfcpp" "${ROOT}/patches/rexglue-0.10.0-utfcpp.patch"
fi

if [[ "$(uname -s)" == "Darwin" && -x "${ROOT}/scripts/build_moltenvk.sh" ]]; then
  "${ROOT}/scripts/build_moltenvk.sh"
fi

# Apply community engine fixes (Ambient Occlusion & Occlusion Query fixes)
if [[ -f "${RUNTIME_DIR}/scripts/patch_game_code.py" && -d "${RUNTIME_DIR}/generated/default" ]]; then
  echo "==> Applying engine fixes (Ambient Occlusion & Occlusion Query fixes)"
  python3 "${RUNTIME_DIR}/scripts/patch_game_code.py" "${RUNTIME_DIR}/generated/default" || true
fi

echo "==> Configuring"
(cd "${RUNTIME_DIR}" && cmake --preset "${PRESET}")

if [[ "${PLATFORM}" == "mac-arm64" && (-d "${SDK_SRC}/.git" || -f "${SDK_SRC}/.git") ]]; then
  echo "==> Building the code generator"
  (cd "${RUNTIME_DIR}" && cmake --build --preset "${PRESET}-${CONFIG}" --target rexglue)
  readonly CODEGEN_STAMP="${RUNTIME_DIR}/generated/default/codegen.build.stamp"
  readonly GENERATOR="$(find "${SDK_SRC}/out" -name 'rexglue*' -type f -perm -u+x -newer "${CODEGEN_STAMP}" 2>/dev/null | head -n 1)"
  if [[ ! -f "${CODEGEN_STAMP}" || -n "${GENERATOR}" ]]; then
    echo "==> Code generator changed: regenerating the recompiled code"
    rm -f "${CODEGEN_STAMP}"
  fi
fi

echo "==> Building (${CONFIG})"
(cd "${RUNTIME_DIR}" && cmake --build --preset "${PRESET}-${CONFIG}" --target azura)

if [[ "$(uname -s)" == "Darwin" ]]; then
  CONFIG_DIR="RelWithDebInfo"
  case "${CONFIG}" in
    debug) CONFIG_DIR="Debug" ;;
    release) CONFIG_DIR="Release" ;;
  esac
  readonly STAGED_MVK="${RUNTIME_DIR}/out/build/${PRESET}/${CONFIG_DIR}/vulkan/lib/libMoltenVK.dylib"
  if [[ -f "${STAGED_MVK}" ]]; then
    readonly UNPATCHED_MVK_HASH="$(awk '/^\[moltenvk\]/{s=1;next} /^\[/{s=0} s && /^unpatched_sha256[[:space:]]*=/{v=$0;sub(/^[^"]*"/,"",v);sub(/".*$/,"",v);print v;exit}' "${REGISTRY}")"
    readonly STAGED_MVK_HASH="$(shasum -a 256 "${STAGED_MVK}" | cut -d ' ' -f 1)"
    if [[ "${STAGED_MVK_HASH}" == "${UNPATCHED_MVK_HASH}" ]]; then
      echo "Error: staged ${STAGED_MVK} matches unpatched MoltenVK (${UNPATCHED_MVK_HASH})!" >&2
      exit 1
    fi
  fi
fi

# Stage runtime to launcher app data
if [[ "$(uname -s)" == "Darwin" ]]; then
  readonly APP_DATA_DIR="${HOME}/Library/Application Support/com.gears3azura.launcher"
else
  readonly APP_DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/com.gears3azura.launcher"
fi
readonly TARGET_RUNTIME_DIR="${APP_DATA_DIR}/runtime"

BUILT_BIN="$(find "${RUNTIME_DIR}/out/build/${PRESET}" -name "azura" -type f 2>/dev/null | head -n 1 || true)"
if [[ -n "${BUILT_BIN}" && -f "${BUILT_BIN}" ]]; then
  echo "==> Staging runtime to launcher app data: ${TARGET_RUNTIME_DIR}"
  mkdir -p "${TARGET_RUNTIME_DIR}"
  cp -rf "$(dirname "${BUILT_BIN}")"/* "${TARGET_RUNTIME_DIR}/"
  if [[ "${PRESET}" == *"-sdk-source"* ]]; then
    CONFIG_NAME="RelWithDebInfo"
    case "${CONFIG}" in
      debug) CONFIG_NAME="Debug" ;;
      release) CONFIG_NAME="Release" ;;
    esac
    if [[ -d "${SDK_SRC}/out/${PLATFORM}/${CONFIG_NAME}" ]]; then
      cp -f "${SDK_SRC}/out/${PLATFORM}/${CONFIG_NAME}"/*.so* "${TARGET_RUNTIME_DIR}/" 2>/dev/null || true
      cp -f "${SDK_SRC}/out/${PLATFORM}/${CONFIG_NAME}"/*.so* "$(dirname "${BUILT_BIN}")/" 2>/dev/null || true
    fi
  else
    cp -f "${SDK_DIR}/lib"/*.so "${TARGET_RUNTIME_DIR}/" 2>/dev/null || true
    cp -f "${SDK_DIR}/lib"/*.so "$(dirname "${BUILT_BIN}")/" 2>/dev/null || true
  fi
  chmod +x "${TARGET_RUNTIME_DIR}/azura" 2>/dev/null || true
  echo "==> Runtime successfully installed for the launcher."
fi

echo "==> Done. Output: ${RUNTIME_DIR}/out/build/${PRESET}/"


