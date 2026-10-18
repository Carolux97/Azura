#!/usr/bin/env bash
# Script de inicio y configuración automática para Gears3-Azura Launcher en Linux.
# Verifica e instala dependencias del sistema, ReXGlue SDK y módulos npm.

set -euo pipefail

# Soluciona la pantalla negra de WebKitGTK en Linux con GPUs NVIDIA / Intel / Wayland
export WEBKIT_DISABLE_DMABUF_RENDERER=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="${ROOT}/launcher"

echo "=========================================================="
echo "   Iniciador y Asistente de Configuración (Gears3-Azura)  "
echo "=========================================================="
echo ""

# 1. Detección de arquitectura
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
    echo "❌ Arquitectura no soportada: ${ARCH}. Se requiere x86_64 o arm64." >&2
    exit 1
    ;;
esac

# 2. Comprobar librerías del sistema necesarias para compilar Tauri en Linux
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
  echo "⚠️  Faltan las siguientes dependencias de desarrollo en el sistema:"
  for pkg in "${MISSING_PKGS[@]}"; do
    echo "    - ${pkg}"
  done
  echo ""
  echo "Para compilar el launcher en Linux se requiere instalar estas librerías con apt."
  read -r -p "¿Deseas instalarlas ahora con sudo? (s/N): " respuesta
  case "${respuesta}" in
    [sS]|[yY])
      echo "==> Instalando dependencias del sistema con apt..."
      sudo apt update
      sudo apt install -y "${MISSING_PKGS[@]}"
      ;;
    *)
      echo "⚠️  No se instalaron las librerías. El launcher podría no compilar sin ellas."
      ;;
  esac
else
  echo "✅ Dependencias del sistema: Todas instaladas."
fi
echo ""

# 3. Comprobar dependencias de Node.js en launcher/
if [ ! -d "${LAUNCHER_DIR}/node_modules" ]; then
  echo "==> Instalando dependencias de Node.js en launcher/..."
  if command -v npm >/dev/null 2>&1; then
    (cd "${LAUNCHER_DIR}" && npm install)
  elif command -v corepack >/dev/null 2>&1; then
    (cd "${LAUNCHER_DIR}" && corepack npm install)
  else
    echo "❌ No se encontró npm ni corepack para instalar las dependencias web." >&2
    exit 1
  fi
  echo "✅ Dependencias de Node.js instaladas correctamente."
else
  echo "✅ Dependencias de Node.js: Listas."
fi
echo ""

# 4. Comprobar ReXGlue SDK (necesario para recompilar el juego y el runtime)
if [ ! -x "${SDK_DIR}/bin/rexglue" ]; then
  echo "⚠️  ReXGlue SDK 0.10.0 no encontrado en: ${SDK_DIR}"
  read -r -p "¿Deseas descargarlo y configurarlo automáticamente desde GitHub? (s/N): " respuesta_sdk
  case "${respuesta_sdk}" in
    [sS]|[yY])
      echo "==> Descargando ReXGlue SDK para ${SDK_ARCH}..."
      mkdir -p "${ROOT}/sdk"
      ZIP_NAME="rexglue-sdk-0.10.0-${SDK_ARCH}.zip"
      ZIP_URL="https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/${ZIP_NAME}"
      TARGET_DIR="${ROOT}/sdk/rexglue-0.10.0-${SDK_ARCH}"

      curl -fSL "${ZIP_URL}" -o "${ROOT}/sdk/${ZIP_NAME}"
      echo "==> Descomprimiendo en ${TARGET_DIR}..."
      mkdir -p "${TARGET_DIR}"
      unzip -q -o "${ROOT}/sdk/${ZIP_NAME}" -d "${TARGET_DIR}"
      rm -f "${ROOT}/sdk/${ZIP_NAME}"
      chmod +x "${TARGET_DIR}/${SDK_ARCH}/bin/rexglue" 2>/dev/null || true
      echo "✅ ReXGlue SDK configurado correctamente."
      ;;
    *)
      echo "ℹ️  Omitiendo descarga del SDK. Puedes descargarlo manualmente más tarde si deseas recompilar el juego."
      ;;
  esac
else
  echo "✅ ReXGlue SDK: Instalado y listo en ${SDK_DIR}"
fi
echo ""

# 5. Comprobar y compilar el Runtime automáticamente si default.xex está listo
TARGET_AZURA="${HOME}/.local/share/com.gears3azura.launcher/runtime/azura"
if [ ! -f "${TARGET_AZURA}" ]; then
  if [ ! -f "${ROOT}/runtime/game/default.xex" ]; then
    FOUND_ISO="$(find "${HOME}/Escritorio" "${HOME}/Descargas" "/run/media" -maxdepth 3 -name "*Spain*.iso" -o -name "*Gears*.iso" 2>/dev/null | head -n 1 || true)"
    if [ -z "${FOUND_ISO}" ]; then
      FOUND_ISO="$(find "${HOME}/Escritorio" "${HOME}/Descargas" -maxdepth 3 -name "*.iso" 2>/dev/null | head -n 1 || true)"
    fi
    if [ -n "${FOUND_ISO}" ]; then
      echo "==> Detectada imagen ISO española en: ${FOUND_ISO}"
      echo "==> Extrayendo default.xex automáticamente..."
      mkdir -p "${ROOT}/runtime/game"
      if [ -x "${ROOT}/tools/extract-xiso" ]; then
        (cd "${ROOT}/runtime/game" && "${ROOT}/tools/extract-xiso" -x -q "${FOUND_ISO}" 2>/dev/null) || true
        # Si se extrajo en subcarpeta o raíz
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
        echo "✅ default.xex (Edición España) extraído correctamente."
      fi
    fi
  fi

  if [ -f "${ROOT}/runtime/game/default.xex" ] && command -v clang >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
    echo "=========================================================="
    echo "   Compilando Runtime azura para Linux...                "
    echo "=========================================================="
    "${ROOT}/scripts/build.sh" || echo "⚠️  No se pudo completar la compilación del runtime automáticamente."
    echo ""
  fi
fi

# 6. Crear acceso directo de escritorio en Linux (menú de aplicaciones)
DESKTOP_DIR="${HOME}/.local/share/applications"
if [ -d "${DESKTOP_DIR}" ] && [ ! -f "${DESKTOP_DIR}/azura-launcher.desktop" ]; then
  cat <<EOF > "${DESKTOP_DIR}/azura-launcher.desktop"
[Desktop Entry]
Version=1.0
Type=Application
Name=Gears of War 3 (Azura Launcher)
Comment=Launcher nativo para Gears of War 3 en Linux
Exec=${ROOT}/iniciar-linux.sh
Icon=${ROOT}/launcher/src-tauri/icons/icon.png
Terminal=false
Categories=Game;
EOF
  chmod +x "${DESKTOP_DIR}/azura-launcher.desktop" 2>/dev/null || true
  echo "✅ Acceso directo creado en el menú de aplicaciones de Ubuntu."
  echo ""
fi

# 6. Iniciar el launcher
echo "=========================================================="
echo "   Iniciando Gears3-Azura Launcher...                    "
echo "=========================================================="
cd "${LAUNCHER_DIR}"
if [ -x "${LAUNCHER_DIR}/src-tauri/target/debug/azura-launcher" ]; then
  exec "${LAUNCHER_DIR}/src-tauri/target/debug/azura-launcher"
elif command -v npx >/dev/null 2>&1; then
  npx tauri dev
elif command -v corepack >/dev/null 2>&1; then
  corepack npx tauri dev
else
  npm run dev:tauri
fi
