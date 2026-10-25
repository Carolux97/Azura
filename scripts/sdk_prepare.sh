# shellcheck shell=bash
# Sets up the ReXGlue SDK for this machine, for scripts/build.sh and scripts/build-kit.sh: the
# platform, the prebuilt SDK (still used for the Vulkan headers) and the SDK source tree the runtime
# builds from, with our patches applied and, on macOS, the patched MoltenVK built. Sourced; needs
# ROOT and SDK_SRC set. Defines PLATFORM, SDK_DIR, sha256(), require_sdk() and prepare_sdk().

# shellcheck source=sdk_patches.sh
source "${ROOT}/scripts/sdk_patches.sh"

# Platform and prebuilt SDK of this machine (the platforms of runtime/CMakePresets.json).
case "$(uname -s):$(uname -m)" in
  Darwin:arm64) readonly PLATFORM="mac-arm64" ;;
  Linux:x86_64|Linux:amd64) readonly PLATFORM="linux-amd64" ;;
  Linux:aarch64|Linux:arm64) readonly PLATFORM="linux-arm64" ;;
  # Git for Windows' bash (MINGW64) or MSYS2.
  MINGW64_NT*:x86_64|MSYS_NT*:x86_64) readonly PLATFORM="win-amd64" ;;
  *)
    echo "Unsupported build platform: $(uname -s) $(uname -m)." >&2
    exit 1
    ;;
esac
readonly SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-${PLATFORM}/${PLATFORM}"
if [[ "${PLATFORM}" == win-* ]]; then
  readonly REXGLUE="${SDK_DIR}/bin/rexglue.exe"
else
  readonly REXGLUE="${SDK_DIR}/bin/rexglue"
fi

# Optional pkg-config files for the build machine (on Linux, for libraries a distribution may
# lack), in sdk/compat/pkgconfig when present.
if [[ -d "${ROOT}/sdk/compat/pkgconfig" ]]; then
  export PKG_CONFIG_PATH="${ROOT}/sdk/compat/pkgconfig:${PKG_CONFIG_PATH:-}"
fi

# SHA-256 of a file: macOS ships shasum, most Linux distributions sha256sum.
sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 "$1" | cut -d ' ' -f 1
  fi
}

# Stops unless both SDK trees are where the project expects them.
require_sdk() {
  if [[ ! -x "${REXGLUE}" ]]; then
    echo "ReXGlue SDK not found at ${SDK_DIR}. See the README for how to install it." >&2
    exit 1
  fi

  if [[ ! -d "${SDK_SRC}/.git" && ! -f "${SDK_SRC}/.git" ]]; then
    echo "ReXGlue SDK source not found at ${SDK_SRC}. Get it with:" >&2
    echo "  git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git ${SDK_SRC}" >&2
    exit 1
  fi
}

# Applies a patch to a git checkout unless it is already applied. Stops when it applies neither
# way, which means the checkout has other changes in the same lines, such as the patches of
# another version of the project.
apply_patch() {
  local dir="$1" patch="$2"
  if git -C "${dir}" apply --reverse --check "${patch}" 2>/dev/null; then
    return
  fi
  if ! git -C "${dir}" apply --check "${patch}" 2>/dev/null; then
    echo "Cannot apply $(basename "${patch}") to ${dir}: the tree has conflicting changes." >&2
    echo "If it has the patches of an earlier version of the project, restore the SDK source" >&2
    echo "(git checkout and git clean in it and in thirdparty/utfcpp, which also removes any" >&2
    echo "changes of your own there) and build again." >&2
    exit 1
  fi
  echo "==> Applying $(basename "${patch}")"
  git -C "${dir}" apply "${patch}"
}

# Applies our patches to the SDK source tree (only when they changed since it was last patched, see
# sdk_patches.sh) and, on macOS, builds the patched MoltenVK into the prebuilt SDK.
prepare_sdk() {
  local patches_hash entry
  patches_hash="$(sdk_patches_hash)"
  if [[ -f "${SDK_PATCHES_STAMP}" && "$(cat "${SDK_PATCHES_STAMP}")" == "${patches_hash}" ]]; then
    echo "==> SDK patches already applied"
  else
    rm -f "${SDK_PATCHES_STAMP}"
    for entry in "${SDK_PATCHES[@]}"; do
      apply_patch "${entry%%|*}" "${entry#*|}"
    done
    echo "${patches_hash}" > "${SDK_PATCHES_STAMP}"
  fi

  if [[ "${PLATFORM}" == mac-* ]]; then
    "${ROOT}/scripts/build_moltenvk.sh"
  fi
}
