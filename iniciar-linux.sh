#!/usr/bin/env bash
# Starts the Gears3-Azura launcher from a checkout on Linux, setting up what it needs first:
# the system packages, the ReXGlue SDK and the npm modules.

set -euo pipefail

# Avoids the black WebKitGTK window on Linux with NVIDIA or Intel GPUs and on Wayland.
export WEBKIT_DISABLE_DMABUF_RENDERER=1

# Runs on the dedicated NVIDIA GPU on hybrid laptops (PRIME render offload), for full frame rate and
# correct Vulkan shaders.
export __NV_PRIME_RENDER_OFFLOAD=1
export __GLX_VENDOR_LIBRARY_NAME=nvidia
export __VK_LAYER_NV_optimus=NVIDIA_only
if [ -f "/usr/share/vulkan/icd.d/nvidia_icd.json" ]; then
  export VK_DRIVER_FILES="/usr/share/vulkan/icd.d/nvidia_icd.json"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="${ROOT}/launcher"

echo "=========================================================="
echo "        Gears3-Azura launcher: start and set-up           "
echo "=========================================================="
echo ""

# 1. Architecture.
ARCH="$(uname -m)"
case "${ARCH}" in
  x86_64|amd64)
    SDK_ARCH="linux-amd64"
    SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-linux-amd64/linux-amd64"
    ;;
  aarch64|arm64)
    SDK_ARCH="linux-arm64"
    SDK_DIR="${ROOT}/sdk/rexglue-0.10.0-linux-arm64/linux-arm64"
    ;;
  *)
    echo "❌ Unsupported architecture: ${ARCH}. x86_64 or arm64 is needed." >&2
    exit 1
    ;;
esac

# 2. The system libraries Tauri needs to build on Linux.
REQUIRED_PKGS=(
  "libwebkit2gtk-4.1-dev"
  "libgtk-3-dev"
  "libayatana-appindicator3-dev"
  "librsvg2-dev"
  "libssl-dev"
  "libdbus-1-dev"
  "npm"
  "clang"
  "ninja-build"
  "7zip"
)

MISSING_PKGS=()
for pkg in "${REQUIRED_PKGS[@]}"; do
  if ! dpkg -s "${pkg}" >/dev/null 2>&1; then
    MISSING_PKGS+=("${pkg}")
  fi
done

if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
  echo "⚠️  These development packages are missing:"
  for pkg in "${MISSING_PKGS[@]}"; do
    echo "    - ${pkg}"
  done
  echo ""
  echo "The launcher needs them to build on Linux; they are installed with apt."
  read -r -p "Install them now with sudo? (y/N): " answer
  case "${answer}" in
    [sS]|[yY])
      echo "==> Installing the system packages with apt..."
      sudo apt update
      sudo apt install -y "${MISSING_PKGS[@]}"
      ;;
    *)
      echo "⚠️  The packages were not installed. The launcher may not build without them."
      ;;
  esac
else
  echo "✅ System packages: all installed."
fi
echo ""

# 3. The Node.js modules of launcher/.
if [ ! -d "${LAUNCHER_DIR}/node_modules" ]; then
  echo "==> Installing the Node.js modules in launcher/..."
  if command -v npm >/dev/null 2>&1; then
    (cd "${LAUNCHER_DIR}" && npm install)
  elif command -v corepack >/dev/null 2>&1; then
    (cd "${LAUNCHER_DIR}" && corepack npm install)
  else
    echo "❌ Neither npm nor corepack was found to install the Node.js modules." >&2
    exit 1
  fi
  echo "✅ Node.js modules installed."
else
  echo "✅ Node.js modules: ready."
fi
echo ""

# 4. The ReXGlue SDK, needed to recompile the game and build the runtime.
if [ ! -x "${SDK_DIR}/bin/rexglue" ]; then
  echo "⚠️  ReXGlue SDK 0.10.0 not found in: ${SDK_DIR}"
  read -r -p "Download and set it up from GitHub now? (y/N): " answer_sdk
  case "${answer_sdk}" in
    [sS]|[yY])
      echo "==> Downloading the ReXGlue SDK for ${SDK_ARCH}..."
      mkdir -p "${ROOT}/sdk"
      ZIP_NAME="rexglue-sdk-0.10.0-${SDK_ARCH}.zip"
      ZIP_URL="https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/${ZIP_NAME}"
      TARGET_DIR="${ROOT}/sdk/rexglue-0.10.0-${SDK_ARCH}"

      curl -fSL "${ZIP_URL}" -o "${ROOT}/sdk/${ZIP_NAME}"
      echo "==> Unpacking it into ${TARGET_DIR}..."
      mkdir -p "${TARGET_DIR}"
      unzip -q -o "${ROOT}/sdk/${ZIP_NAME}" -d "${TARGET_DIR}"
      rm -f "${ROOT}/sdk/${ZIP_NAME}"
      chmod +x "${TARGET_DIR}/${SDK_ARCH}/bin/rexglue" 2>/dev/null || true
      echo "✅ ReXGlue SDK set up."
      ;;
    *)
      echo "ℹ️  Skipping the SDK download. You can download it by hand later to recompile the game."
      ;;
  esac
else
  echo "✅ ReXGlue SDK: ready in ${SDK_DIR}"
fi
echo ""

# 5. Builds the runtime when default.xex is there (or can be found in a disc image).
TARGET_AZURA="${HOME}/.local/share/com.gears3azura.launcher/runtime/azura"
if [ ! -f "${TARGET_AZURA}" ]; then
  if [ ! -f "${ROOT}/runtime/game/default.xex" ]; then
    FOUND_ISO="$(find "${HOME}/Escritorio" "${HOME}/Descargas" "/run/media" -maxdepth 3 -name "*Spain*.iso" -o -name "*Gears*.iso" 2>/dev/null | head -n 1 || true)"
    if [ -z "${FOUND_ISO}" ]; then
      FOUND_ISO="$(find "${HOME}/Escritorio" "${HOME}/Descargas" -maxdepth 3 -name "*.iso" 2>/dev/null | head -n 1 || true)"
    fi
    if [ -n "${FOUND_ISO}" ]; then
      echo "==> Disc image found: ${FOUND_ISO}"
      echo "==> Extracting default.xex from it..."
      mkdir -p "${ROOT}/runtime/game"
      if [ -x "${ROOT}/tools/extract-xiso" ]; then
        (cd "${ROOT}/runtime/game" && "${ROOT}/tools/extract-xiso" -x -q "${FOUND_ISO}" 2>/dev/null) || true
        # Wherever it was extracted, at the top or in a subfolder.
        find "${ROOT}/runtime/game" -name "default.xex" -exec mv -f {} "${ROOT}/runtime/game/default.xex" \; 2>/dev/null || true
      fi
      if [ ! -f "${ROOT}/runtime/game/default.xex" ]; then
        python3 -c "
with open('${FOUND_ISO}', 'rb') as fin:
    fin.seek(0xdc68000)
    data = fin.read(21938176)
    with open('${ROOT}/runtime/game/default.xex', 'wb') as fout:
        fout.write(data)
" 2>/dev/null || true
      fi
      if [ -f "${ROOT}/runtime/game/default.xex" ]; then
        echo "✅ default.xex extracted."
      fi
    fi
  fi

  if [ -f "${ROOT}/runtime/game/default.xex" ] && command -v clang >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
    echo "=========================================================="
    echo "   Building the azura runtime for Linux...                "
    echo "=========================================================="
    "${ROOT}/scripts/build.sh" || echo "⚠️  The runtime could not be built."
    echo ""
  fi
fi

# 6. A desktop entry in the applications menu.
DESKTOP_DIR="${HOME}/.local/share/applications"
if [ -d "${DESKTOP_DIR}" ] && [ ! -f "${DESKTOP_DIR}/azura-launcher.desktop" ]; then
  cat <<EOF > "${DESKTOP_DIR}/azura-launcher.desktop"
[Desktop Entry]
Version=1.0
Type=Application
Name=Gears of War 3 (Azura Launcher)
Comment=Native launcher for Gears of War 3 on Linux
Exec=${ROOT}/iniciar-linux.sh
Icon=${ROOT}/launcher/src-tauri/icons/icon.png
Terminal=false
Categories=Game;
EOF
  chmod +x "${DESKTOP_DIR}/azura-launcher.desktop" 2>/dev/null || true
  echo "✅ Desktop entry added to the applications menu."
  echo ""
fi

# 7. Start the launcher.
echo "=========================================================="
echo "   Starting the Gears3-Azura launcher...                  "
echo "=========================================================="
cd "${LAUNCHER_DIR}"
if [ -x "${LAUNCHER_DIR}/src-tauri/target/release/azura-launcher" ]; then
  exec "${LAUNCHER_DIR}/src-tauri/target/release/azura-launcher"
elif [ -x "${LAUNCHER_DIR}/src-tauri/target/debug/azura-launcher" ]; then
  exec "${LAUNCHER_DIR}/src-tauri/target/debug/azura-launcher"
elif command -v npx >/dev/null 2>&1; then
  npx tauri dev
elif command -v corepack >/dev/null 2>&1; then
  corepack npx tauri dev
else
  npm run dev:tauri
fi
