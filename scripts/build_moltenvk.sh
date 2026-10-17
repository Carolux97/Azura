#!/usr/bin/env bash
# Builds a patched libMoltenVK.dylib for macOS (Apple Silicon) to replace the unpatched 1.4.1 copy
# shipped with the prebuilt ReXGlue SDK.
#
# Why this exists:
#   MoltenVK 1.4.1 has an ordering bug in MVKSwapchain::initCAMetalLayer that leaves the Metal
#   layer's drawableSize at 1x1 whenever a swapchain is recreated (e.g. switching to fullscreen),
#   causing a permanent black screen while audio keeps playing. Building only MoltenVK from its
#   pinned commit with patches/moltenvk-*.patch applied lets us keep using the fast prebuilt
#   ReXGlue SDK while shipping the fixed MoltenVK binary.
#
# Steps:
#   1. read the pinned MoltenVK commit and unpatched SHA-256 from versions.toml;
#   2. locate or clone MoltenVK at that commit (reusing sdk/rexglue-0.10.0-src/thirdparty/moltenvk
#      when present, otherwise cloning into sdk/moltenvk-src);
#   3. reset the MoltenVK checkout to the pinned commit and apply patches/moltenvk-*.patch;
#   4. build MoltenVK with CMake + Ninja using the VulkanHeaders shipped in the prebuilt SDK
#      (no full Xcode installation required — Command Line Tools are sufficient);
#   5. copy the resulting libMoltenVK.dylib to sdk/patched-moltenvk/libMoltenVK.dylib, write a
#      stamp file with the commit + patch hashes, and install it into the prebuilt SDK folder so
#      CMake staging picks it up automatically.
#
# Usage: scripts/build_moltenvk.sh [--force]

set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly REGISTRY="${ROOT}/versions.toml"
readonly SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-mac-arm64/mac-arm64"
readonly PATCH_DIR="${ROOT}/patches"
readonly OUT_DIR="${ROOT}/sdk/patched-moltenvk"
readonly OUT_DYLIB="${OUT_DIR}/libMoltenVK.dylib"
readonly STAMP_FILE="${OUT_DIR}/build.stamp"

FORCE=false
case "${1:-}" in
  "") ;;
  --force) FORCE=true ;;
  *)
    echo "Unknown option '$1'. Usage: scripts/build_moltenvk.sh [--force]" >&2
    exit 1
    ;;
esac

# Reads a key from the [moltenvk] section of versions.toml.
moltenvk_field() {
  awk -v key="$1" '
    /^\[moltenvk\]/ { in_sec = 1; next }
    /^\[/ { in_sec = 0 }
    in_sec && $0 ~ "^" key "[[:space:]]*=" {
      val = $0
      sub(/^[^"]*"/, "", val)
      sub(/".*$/, "", val)
      print val
      exit
    }
  ' "${REGISTRY}"
}

readonly MVK_REV="$(moltenvk_field revision)"
readonly UNPATCHED_HASH="$(moltenvk_field unpatched_sha256)"
if [[ -z "${MVK_REV}" || -z "${UNPATCHED_HASH}" ]]; then
  echo "Missing [moltenvk] revision or unpatched_sha256 in ${REGISTRY}." >&2
  exit 1
fi

shopt -s nullglob
readonly PATCHES=("${PATCH_DIR}"/moltenvk-*.patch)
shopt -u nullglob
if [[ ${#PATCHES[@]} -eq 0 ]]; then
  echo "No MoltenVK patches found in ${PATCH_DIR}/moltenvk-*.patch" >&2
  exit 1
fi

# Compute a stamp string from the pinned revision and the SHA-256 of every patch file.
PATCH_HASHES="$(shasum -a 256 "${PATCHES[@]}" | awk '{print $1}' | tr '\n' ':')"
readonly EXPECTED_STAMP="rev=${MVK_REV};patches=${PATCH_HASHES}"

if ! ${FORCE} && [[ -f "${OUT_DYLIB}" && -f "${STAMP_FILE}" ]]; then
  CURRENT_STAMP="$(cat "${STAMP_FILE}")"
  if [[ "${CURRENT_STAMP}" == "${EXPECTED_STAMP}" ]]; then
    CURRENT_HASH="$(shasum -a 256 "${OUT_DYLIB}" | cut -d ' ' -f 1)"
    if [[ "${CURRENT_HASH}" == "${UNPATCHED_HASH}" ]]; then
      echo "Refusing cached ${OUT_DYLIB}: SHA-256 matches unpatched MoltenVK!" >&2
      exit 1
    fi
    # Ensure the prebuilt SDK also has the patched copy installed.
    if [[ -d "${SDK_DIR}/lib" ]]; then
      cp -f "${OUT_DYLIB}" "${SDK_DIR}/lib/libMoltenVK.dylib"
    fi
    echo "==> Patched MoltenVK is up to date (${OUT_DYLIB})"
    exit 0
  fi
fi

readonly SDK_VULKAN_HEADERS="${SDK_DIR}/share/cmake/VulkanHeaders/VulkanHeadersConfig.cmake"
if [[ ! -f "${SDK_VULKAN_HEADERS}" ]]; then
  echo "VulkanHeadersConfig.cmake not found at ${SDK_VULKAN_HEADERS}." >&2
  echo "Install the ReXGlue SDK at ${SDK_DIR} first." >&2
  exit 1
fi

# Prefer the submodule in sdk/rexglue-0.10.0-src if available; otherwise use sdk/moltenvk-src.
if [[ -d "${ROOT}/sdk/rexglue-0.10.0-src/thirdparty/moltenvk/.git" || -f "${ROOT}/sdk/rexglue-0.10.0-src/thirdparty/moltenvk/.git" ]]; then
  MVK_SRC="${ROOT}/sdk/rexglue-0.10.0-src/thirdparty/moltenvk"
else
  MVK_SRC="${ROOT}/sdk/moltenvk-src"
  if [[ ! -d "${MVK_SRC}/.git" ]]; then
    echo "==> Cloning MoltenVK (${MVK_REV})"
    mkdir -p "$(dirname "${MVK_SRC}")"
    git clone https://github.com/KhronosGroup/MoltenVK.git "${MVK_SRC}"
  fi
fi

echo "==> Preparing MoltenVK source at ${MVK_REV}"
git -C "${MVK_SRC}" checkout -f "${MVK_REV}"
git -C "${MVK_SRC}" clean -fd

for patch in "${PATCHES[@]}"; do
  echo "==> Applying $(basename "${patch}")"
  git -C "${MVK_SRC}" apply "${patch}"
done

readonly BUILD_DIR="${OUT_DIR}/build"
mkdir -p "${BUILD_DIR}"

echo "==> Configuring MoltenVK (Release, arm64, macOS 13.3)"
cmake -S "${MVK_SRC}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 \
  -DMVK_BUILD_SHADER_CONVERTER_TOOL=OFF \
  -DMVK_EXCLUDE_SPIRV_TOOLS=ON \
  -DCMAKE_PROJECT_MoltenVK_INCLUDE="${SDK_VULKAN_HEADERS}"

echo "==> Building MoltenVK"
cmake --build "${BUILD_DIR}" --target MoltenVK

readonly BUILT_DYLIB="${BUILD_DIR}/MoltenVK/libMoltenVK.dylib"
if [[ ! -f "${BUILT_DYLIB}" ]]; then
  echo "Expected output ${BUILT_DYLIB} was not produced." >&2
  exit 1
fi

readonly BUILT_HASH="$(shasum -a 256 "${BUILT_DYLIB}" | cut -d ' ' -f 1)"
if [[ "${BUILT_HASH}" == "${UNPATCHED_HASH}" ]]; then
  echo "Error: built MoltenVK matches unpatched SHA-256 (${UNPATCHED_HASH})!" >&2
  exit 1
fi

cp -f "${BUILT_DYLIB}" "${OUT_DYLIB}"
printf '%s\n' "${EXPECTED_STAMP}" > "${STAMP_FILE}"

# Back up the original unpatched SDK dylib once, then overwrite libMoltenVK.dylib in the SDK so any
# CMake preset using the prebuilt SDK stages the patched library automatically.
if [[ -f "${SDK_DIR}/lib/libMoltenVK.dylib" ]]; then
  EXISTING_SDK_HASH="$(shasum -a 256 "${SDK_DIR}/lib/libMoltenVK.dylib" | cut -d ' ' -f 1)"
  if [[ "${EXISTING_SDK_HASH}" == "${UNPATCHED_HASH}" && ! -f "${SDK_DIR}/lib/libMoltenVK.unpatched.dylib" ]]; then
    cp -f "${SDK_DIR}/lib/libMoltenVK.dylib" "${SDK_DIR}/lib/libMoltenVK.unpatched.dylib"
  fi
  cp -f "${OUT_DYLIB}" "${SDK_DIR}/lib/libMoltenVK.dylib"
fi

echo "==> Built and installed patched MoltenVK (SHA-256: ${BUILT_HASH})"
