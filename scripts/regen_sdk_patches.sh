#!/usr/bin/env bash
# Regenerates the two SDK patches that hold our GPU work from the working SDK tree:
# - patches/rexglue-0.10.0-metal-backend.patch: every change to the SDK's files (the Metal
#   backend, and the changes to common and Vulkan files it and the shader compiler need);
# - patches/rexglue-0.10.0-xenos-shader-compiler.patch: the new files of the Xenos shader
#   compiler (src/graphics/pipeline/shader/xsc/).
# The base is the SDK commit with the patches that come before them, rebuilt in a temporary
# folder, so nothing outside the repository is needed. Both patches are checked to apply in
# reverse to the working tree afterwards. Usage: scripts/regen_sdk_patches.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ROOT}/sdk/rexglue-0.10.0-src"
METAL_PATCH="${ROOT}/patches/rexglue-0.10.0-metal-backend.patch"
XSC_PATCH="${ROOT}/patches/rexglue-0.10.0-xenos-shader-compiler.patch"
XSC_DIR="src/graphics/pipeline/shader/xsc"
# The patches build.sh applies before these two, on top of the SDK commit.
EARLIER_PATCHES=(
  rexglue-0.10.0-thread-cpu.patch
  rexglue-0.10.0-gpu-profiling.patch
  rexglue-0.10.0-present-pipeline.patch
  rexglue-0.10.0-edram-sample-space.patch
  rexglue-0.10.0-input-race.patch
)

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sdk_patches.XXXXXX")"
trap 'rm -rf "${WORK:?}"' EXIT

echo "==> Rebuilding the base SDK tree"
mkdir -p "${WORK}/base"
git -C "${SDK}" archive HEAD | tar -x -C "${WORK}/base"
git -C "${WORK}/base" init -q
for patch in "${EARLIER_PATCHES[@]}"; do
  git -C "${WORK}/base" apply "${ROOT}/patches/${patch}"
done

echo "==> Diffing"
# Files that differ or exist only in the working tree, outside build outputs and third-party code.
# diff exits with 1 when the trees differ, which they always do here.
changed=$({ diff -rq "${WORK}/base" "${SDK}" -x .git -x out -x thirdparty -x build || true; } | \
  sed -n "s|^Files ${WORK}/base/\(.*\) and .* differ$|\1|p; s|^Only in ${SDK}/\(.*\): \(.*\)$|\1/\2|p" | \
  sed 's|^/||')
mkdir -p "${WORK}/metal/a" "${WORK}/metal/b" "${WORK}/xsc/a" "${WORK}/xsc/b"
for f in ${changed}; do
  if [[ "${f}" == "${XSC_DIR}"* ]]; then
    side="${WORK}/xsc"
  else
    side="${WORK}/metal"
  fi
  if [[ -d "${SDK}/${f}" ]]; then
    mkdir -p "${side}/b/${f}"
    cp -R "${SDK}/${f}/." "${side}/b/${f}/"
  else
    mkdir -p "${side}/a/$(dirname "${f}")" "${side}/b/$(dirname "${f}")"
    if [[ -f "${WORK}/base/${f}" ]]; then
      cp "${WORK}/base/${f}" "${side}/a/${f}"
    fi
    cp "${SDK}/${f}" "${side}/b/${f}"
  fi
done
(cd "${WORK}/metal" && git diff --no-index --no-prefix a b > "${METAL_PATCH}" || true)
(cd "${WORK}/xsc" && git diff --no-index --no-prefix a b > "${XSC_PATCH}" || true)
# git diff --no-index names new files "b/<path>"; the patches use plain paths like the others.
sed -i.bak 's|^diff --git b/|diff --git a/|' "${METAL_PATCH}" "${XSC_PATCH}"
rm -f "${METAL_PATCH}.bak" "${XSC_PATCH}.bak"

git -C "${SDK}" apply --reverse --check "${METAL_PATCH}"
git -C "${SDK}" apply --reverse --check "${XSC_PATCH}"
echo "==> metal backend patch: $(grep -c '^diff --git' "${METAL_PATCH}") files"
echo "==> xenos shader compiler patch: $(grep -c '^diff --git' "${XSC_PATCH}") files"
