# The ReXGlue SDK patches, shared by build.sh (which applies them) and regen_sdk_patches.sh (which
# makes some of them from the patched tree). Sourced; needs ROOT and SDK_SRC set.

# The patches, in the order they apply, as "folder|patch" pairs.
SDK_PATCHES=(
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-exit-code.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-thread-cpu.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-gpu-profiling.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-present-pipeline.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-edram-sample-space.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-input-race.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-kernel-random.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-metal-backend.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-xenos-shader-compiler.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-ppc-interpreter.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-codegen-stamp.patch"
  "${SDK_SRC}|${ROOT}/patches/rexglue-0.10.0-hle-gfx.patch"
  "${SDK_SRC}/thirdparty/utfcpp|${ROOT}/patches/rexglue-0.10.0-utfcpp.patch"
  # The SDK builds MoltenVK from this tree on macOS, so its fixes go here too, or a build of the SDK
  # (ours, or the player kit's) ships the MoltenVK without them.
  "${SDK_SRC}/thirdparty/moltenvk|${ROOT}/patches/moltenvk-1.4.1-encoder-labels.patch"
  "${SDK_SRC}/thirdparty/moltenvk|${ROOT}/patches/moltenvk-1.4.1-swapchain-1x1.patch"
)

# Hash of the patches the SDK tree was last patched with, beside the tree (sdk/ is not in git).
# While it matches the patches, build.sh does not check them one by one: that only works while no
# later patch changes the lines around an earlier one's, and once one does, the earlier patch
# neither applies nor reverts on the patched tree.
SDK_PATCHES_STAMP="${SDK_SRC}.patches.sha256"

# Prints the hash of the contents of every patch, in order.
sdk_patches_hash() {
  local entry
  for entry in "${SDK_PATCHES[@]}"; do
    cat "${entry#*|}"
  done | { if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi; } |
    cut -d ' ' -f 1
}
