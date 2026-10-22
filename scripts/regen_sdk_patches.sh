#!/usr/bin/env bash
# Regenerates the SDK patches that hold our own work from the working SDK tree:
# - patches/rexglue-0.10.0-metal-backend.patch: every change to the SDK's files not listed below
#   (the Metal backend, and the changes to common and Vulkan files it and the shader compiler
#   need);
# - patches/rexglue-0.10.0-xenos-shader-compiler.patch: the new files of the Xenos shader
#   compiler (src/graphics/pipeline/shader/xsc/);
# - patches/rexglue-0.10.0-ppc-interpreter.patch: the PowerPC interpreter for functions the code
#   generator did not find, its differential bench, and the code generator changes they need (the
#   files in INTERP_PATHS);
# - patches/rexglue-0.10.0-codegen-stamp.patch: codegen re-running when the generator itself
#   changes (the files in STAMP_PATHS).
# Each changed file goes to exactly one patch, so the patches apply in any order. The base is the
# SDK commit with the patches that come before them, rebuilt in a temporary folder, so nothing
# outside the repository is needed. Every patch is checked to apply in reverse to the working tree
# afterwards. Usage: scripts/regen_sdk_patches.sh

set -euo pipefail
# The diff output is parsed below: keep its messages ("Files ... differ", "Only in ...") in English
# whatever the user's language.
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ROOT}/sdk/rexglue-0.10.0-src"
METAL_PATCH="${ROOT}/patches/rexglue-0.10.0-metal-backend.patch"
XSC_PATCH="${ROOT}/patches/rexglue-0.10.0-xenos-shader-compiler.patch"
XSC_DIR="src/graphics/pipeline/shader/xsc"
INTERP_PATCH="${ROOT}/patches/rexglue-0.10.0-ppc-interpreter.patch"
STAMP_PATCH="${ROOT}/patches/rexglue-0.10.0-codegen-stamp.patch"
# Files of the codegen stamp patch.
STAMP_PATHS=(
  resources/templates/init/rexglue_cmake.inja
  src/codegen/project_recompiler.cpp
)
# Files and folders of the interpreter patch.
INTERP_PATHS=(
  include/rex/ppc/context_access.h
  include/rex/system/ppc_interpreter.h
  src/system/ppc_interpreter.cpp
  src/system/ppc_interpreter_vector.inc
  src/system/function_dispatcher.cpp
  src/system/CMakeLists.txt
  src/codegen/builders.h
  src/codegen/builders/builder_context.h
  src/codegen/builders/context.cpp
  src/codegen/builders/control_flow.cpp
  src/codegen/codegen_writer.cpp
  src/codegen/phase_validate.cpp
  src/codegen/instruction_dispatch.cpp
  src/codegen/ppc/disasm.cpp
  src/codegen/ppc/disasm.h
  src/rexglue/CMakeLists.txt
  src/rexglue/main.cpp
  src/rexglue/commands/interp_bench.cpp
  src/rexglue/commands/interp_bench.h
  tests/interp_bench
)
# The patches build.sh applies before these two, on top of the SDK commit.
EARLIER_PATCHES=(
  rexglue-0.10.0.patch
  rexglue-0.10.0-exit-code.patch
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
mkdir -p "${WORK}/metal/a" "${WORK}/metal/b" "${WORK}/xsc/a" "${WORK}/xsc/b" \
  "${WORK}/interp/a" "${WORK}/interp/b" "${WORK}/stamp/a" "${WORK}/stamp/b"
# Whether $1 is one of the files or folders given after it.
in_paths() {
  local file="$1" path
  shift
  for path in "$@"; do
    if [[ "${file}" == "${path}" || "${file}" == "${path}/"* ]]; then
      return 0
    fi
  done
  return 1
}
for f in ${changed}; do
  if [[ "${f}" == "${XSC_DIR}"* ]]; then
    side="${WORK}/xsc"
  elif in_paths "${f}" "${INTERP_PATHS[@]}"; then
    side="${WORK}/interp"
  elif in_paths "${f}" "${STAMP_PATHS[@]}"; then
    side="${WORK}/stamp"
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
(cd "${WORK}/interp" && git diff --no-index --no-prefix a b > "${INTERP_PATCH}" || true)
(cd "${WORK}/stamp" && git diff --no-index --no-prefix a b > "${STAMP_PATCH}" || true)
# git diff --no-index names new files "b/<path>"; the patches use plain paths like the others.
sed -i.bak 's|^diff --git b/|diff --git a/|' "${METAL_PATCH}" "${XSC_PATCH}" "${INTERP_PATCH}" \
  "${STAMP_PATCH}"
rm -f "${METAL_PATCH}.bak" "${XSC_PATCH}.bak" "${INTERP_PATCH}.bak" "${STAMP_PATCH}.bak"

git -C "${SDK}" apply --reverse --check "${METAL_PATCH}"
git -C "${SDK}" apply --reverse --check "${XSC_PATCH}"
git -C "${SDK}" apply --reverse --check "${INTERP_PATCH}"
git -C "${SDK}" apply --reverse --check "${STAMP_PATCH}"
echo "==> metal backend patch: $(grep -c '^diff --git' "${METAL_PATCH}") files"
echo "==> xenos shader compiler patch: $(grep -c '^diff --git' "${XSC_PATCH}") files"
echo "==> ppc interpreter patch: $(grep -c '^diff --git' "${INTERP_PATCH}") files"
echo "==> codegen stamp patch: $(grep -c '^diff --git' "${STAMP_PATCH}") files"
