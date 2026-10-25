#!/usr/bin/env bash
# Builds the player kit for this platform: everything the launcher needs to build the runtime on a
# player's machine from their own default.xex, with only a C++ compiler there
# (docs/player-runtime-build.md). Needs no game files, so it runs in CI.
#
# Steps:
#   1. set up the SDK as scripts/build.sh does (our patches, and on macOS the patched MoltenVK);
#   2. build the patched SDK on its own, with its own preset as its releases are built, and install
#      it: the form the kit ships it in;
#   3. configure the runtime against that install with AZURA_KIT_PROBE and build our code
#      (azura_host);
#   4. assemble the kit from it (scripts/kit/make_kit.py): the commands CMake would run to build
#      and stage the runtime, our compiled code, and the parts of the SDK those commands use;
#   5. check it (scripts/kit/check_kit.py): nothing from a game in it, and its commands work.
#
# The result is runtime/out/kit/<platform>/kit, with kit.json describing it.
#
# Usage: scripts/build-kit.sh [debug|relwithdebinfo|release]   (default: relwithdebinfo, as
# scripts/build.sh, so players run the configuration the project tests)

set -euo pipefail

readonly CONFIG="${1:-relwithdebinfo}"
# On Windows (Git Bash) as a Windows path with forward slashes (pwd -W), which the native
# programs it is passed to (CMake, Python, Node) read; elsewhere pwd -W fails and pwd answers.
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && { pwd -W 2>/dev/null || pwd; })"
readonly RUNTIME_DIR="${ROOT}/runtime"
readonly SDK_SRC="${ROOT}/sdk/rexglue-0.10.0-src"
# shellcheck source=sdk_prepare.sh
source "${ROOT}/scripts/sdk_prepare.sh"

case "${CONFIG}" in
  debug) readonly CONFIG_NAME="Debug" ;;
  relwithdebinfo) readonly CONFIG_NAME="RelWithDebInfo" ;;
  release) readonly CONFIG_NAME="Release" ;;
  *)
    echo "Unknown configuration '${CONFIG}'. Use debug, relwithdebinfo or release." >&2
    exit 1
    ;;
esac

readonly OUT="${RUNTIME_DIR}/out/kit/${PLATFORM}"
readonly SDK_BUILD="${OUT}/sdk-build"
readonly SDK_INSTALL="${OUT}/sdk"
readonly PROBE_BUILD="${OUT}/probe-build"
readonly KIT="${OUT}/kit"

# The oldest macOS the kit's code runs on, the same as the SDK's own releases. The launcher
# compiles the player's code for it too (the kit's commands carry it), so every object agrees.
readonly MACOS_DEPLOYMENT_TARGET="13.3"
PLATFORM_ARGS=()
if [[ "${PLATFORM}" == mac-* ]]; then
  PLATFORM_ARGS+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOS_DEPLOYMENT_TARGET}")
fi
if [[ "${PLATFORM}" == win-* ]]; then
  # Vulkan, as on Linux (the SDK defaults to D3D12 on Windows), and the release CRT in every
  # configuration, as the runtime's presets build it, so the SDK and our code share one CRT.
  PLATFORM_ARGS+=("-DREXGLUE_USE_VULKAN=ON" "-DREXGLUE_USE_D3D12=OFF" "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL")
fi

# Python 3: on Windows it is python.exe (python3 there may be the Microsoft Store's placeholder).
if [[ "${PLATFORM}" == win-* ]]; then
  readonly PYTHON="python"
else
  readonly PYTHON="python3"
fi

require_sdk
prepare_sdk

echo "==> Building and installing the patched SDK (${CONFIG_NAME})"
# The SDK's presets name the platform as this project does. Its libraries go to this build's own
# folder (REXGLUE_OUTPUT_DIR), not to the source tree's out/, which scripts/build.sh uses, and
# REXGLUE_REGISTER_PACKAGE=OFF keeps the install out of this machine's CMake package registry: it
# is for the kit, not for this machine.
(cd "${SDK_SRC}" && cmake --preset "${PLATFORM}" -B "${SDK_BUILD}" \
  -DCMAKE_INSTALL_PREFIX="${SDK_INSTALL}" -DREXGLUE_OUTPUT_DIR="${SDK_BUILD}/out" \
  -DREXGLUE_REGISTER_PACKAGE=OFF ${PLATFORM_ARGS[@]+"${PLATFORM_ARGS[@]}"})
cmake --build "${SDK_BUILD}" --config "${CONFIG_NAME}" --target install --parallel

echo "==> Building our code against the installed SDK"
(cd "${RUNTIME_DIR}" && cmake --preset "${PLATFORM}" -B "${PROBE_BUILD}" \
  -DAZURA_KIT_PROBE=ON -DCMAKE_PREFIX_PATH="${SDK_INSTALL}" ${PLATFORM_ARGS[@]+"${PLATFORM_ARGS[@]}"})
cmake --build "${PROBE_BUILD}" --config "${CONFIG_NAME}" --target azura_host --parallel

echo "==> Assembling the kit"
REVISION="$(git -C "${ROOT}" rev-parse --short HEAD)"
if [[ -n "$(git -C "${ROOT}" status --porcelain -- runtime scripts patches)" ]]; then
  REVISION="${REVISION}-modified"
fi
"${PYTHON}" "${ROOT}/scripts/kit/make_kit.py" --probe-build "${PROBE_BUILD}" --config "${CONFIG_NAME}" \
  --sdk "${SDK_INSTALL}" --runtime "${RUNTIME_DIR}" --root "${ROOT}" --out "${KIT}" \
  --platform "${PLATFORM}" --revision "${REVISION}"

echo "==> Checking the kit"
"${PYTHON}" "${ROOT}/scripts/kit/check_kit.py" --kit "${KIT}" --registry "${ROOT}/versions.toml" \
  --work "${OUT}/check"

echo "==> Done. Kit: ${KIT} ($(du -sh "${KIT}" | cut -f 1))"
