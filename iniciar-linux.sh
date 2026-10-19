#!/usr/bin/env bash
# Script de inicio y configuración automática para Gears3-Azura Launcher en Linux.
# Compatible con múltiples distribuciones: Ubuntu/Debian, Arch/Manjaro, Fedora, openSUSE, Void, Alpine, etc.
# Verifica e instala dependencias del sistema, ReXGlue SDK y módulos npm.

set -euo pipefail

# Soluciona la pantalla negra de WebKitGTK en Linux con GPUs NVIDIA / Intel / Wayland
export WEBKIT_DISABLE_DMABUF_RENDERER=1

# Forzar GPU dedicada NVIDIA solo si existe hardware y controlador NVIDIA activo en el equipo
if [ -e "/dev/nvidiactl" ] && [ -f "/usr/share/vulkan/icd.d/nvidia_icd.json" ]; then
  export __NV_PRIME_RENDER_OFFLOAD=1
  export __GLX_VENDOR_LIBRARY_NAME=nvidia
  export __VK_LAYER_NV_optimus=NVIDIA_only
  export VK_DRIVER_FILES="/usr/share/vulkan/icd.d/nvidia_icd.json"
fi

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

# 2. Detección de Distribución y Gestor de Paquetes
DISTRO_NAME="Linux"
PKG_MANAGER=""
INSTALL_CMD=""
DISTRO_PKGS=()

if [ -f /etc/os-release ]; then
  # shellcheck source=/dev/null
  . /etc/os-release
  DISTRO_NAME="${NAME:-Linux}"
  DISTRO_ID="${ID:-}"
  DISTRO_LIKE="${ID_LIKE:-}"
else
  DISTRO_ID=""
  DISTRO_LIKE=""
fi

echo "🐧 Distribución detectada: ${DISTRO_NAME} (${ARCH})"

# Determinar gestor de paquetes y lista de dependencias
if [ "${DISTRO_ID}" = "bazzite" ] || grep -qi "bazzite" /etc/os-release 2>/dev/null || command -v rpm-ostree >/dev/null 2>&1; then
  PKG_MANAGER="rpm-ostree (Bazzite / Fedora Atomic)"
  INSTALL_CMD="rpm-ostree install --apply-live --idempotent"
  DISTRO_PKGS=(
    "pkgconf-pkg-config"
    "webkit2gtk4.1-devel"
    "gtk3-devel"
    "libayatana-appindicator-gtk3-devel"
    "librsvg2-devel"
    "openssl-devel"
    "dbus-devel"
    "nodejs"
    "npm"
    "clang"
    "ninja-build"
    "p7zip"
    "p7zip-plugins"
    "curl"
    "unzip"
  )
elif command -v apt-get >/dev/null 2>&1; then
  PKG_MANAGER="apt"
  INSTALL_CMD="sudo apt update && sudo apt install -y"
  DISTRO_PKGS=(
    "build-essential"
    "pkg-config"
    "libwebkit2gtk-4.1-dev"
    "libgtk-3-dev"
    "libayatana-appindicator3-dev"
    "librsvg2-dev"
    "libssl-dev"
    "libdbus-1-dev"
    "nodejs"
    "npm"
    "clang"
    "ninja-build"
    "p7zip-full"
    "curl"
    "unzip"
  )
elif command -v pacman >/dev/null 2>&1; then
  PKG_MANAGER="pacman"
  INSTALL_CMD="sudo pacman -S --needed --noconfirm"
  DISTRO_PKGS=(
    "base-devel"
    "pkgconf"
    "webkit2gtk-4.1"
    "gtk3"
    "libayatana-appindicator"
    "librsvg"
    "openssl"
    "dbus"
    "nodejs"
    "npm"
    "clang"
    "ninja"
    "p7zip"
    "curl"
    "unzip"
  )
elif command -v dnf >/dev/null 2>&1; then
  PKG_MANAGER="dnf"
  INSTALL_CMD="sudo dnf install -y"
  DISTRO_PKGS=(
    "@development-tools"
    "pkgconf-pkg-config"
    "webkit2gtk4.1-devel"
    "gtk3-devel"
    "libayatana-appindicator-gtk3-devel"
    "librsvg2-devel"
    "openssl-devel"
    "dbus-devel"
    "nodejs"
    "npm"
    "clang"
    "ninja-build"
    "p7zip"
    "p7zip-plugins"
    "curl"
    "unzip"
  )
elif command -v zypper >/dev/null 2>&1; then
  PKG_MANAGER="zypper"
  INSTALL_CMD="sudo zypper install -y"
  DISTRO_PKGS=(
    "patterns-devel-base-devel_basis"
    "pkg-config"
    "webkit2gtk-4_1-devel"
    "gtk3-devel"
    "libayatana-appindicator3-devel"
    "librsvg-devel"
    "libopenssl-devel"
    "dbus-1-devel"
    "nodejs"
    "npm"
    "clang"
    "ninja"
    "p7zip"
    "curl"
    "unzip"
  )
elif command -v xbps-install >/dev/null 2>&1; then
  PKG_MANAGER="xbps"
  INSTALL_CMD="sudo xbps-install -Sy"
  DISTRO_PKGS=(
    "base-devel"
    "pkg-config"
    "webkit2gtk4.1-devel"
    "gtk+3-devel"
    "libayatana-appindicator-devel"
    "librsvg-devel"
    "openssl-devel"
    "dbus-devel"
    "nodejs"
    "clang"
    "ninja"
    "p7zip"
    "curl"
    "unzip"
  )
elif command -v apk >/dev/null 2>&1; then
  PKG_MANAGER="apk"
  INSTALL_CMD="sudo apk add"
  DISTRO_PKGS=(
    "build-base"
    "pkgconf"
    "webkit2gtk-4.1-dev"
    "gtk+3.0-dev"
    "libayatana-appindicator-dev"
    "librsvg-dev"
    "openssl-dev"
    "dbus-dev"
    "nodejs"
    "npm"
    "clang"
    "samurai"
    "7zip"
    "curl"
    "unzip"
  )
fi

# 3. Comprobar librerías del sistema necesarias para Tauri y ReXGlue
MISSING_REASONS=()

# Comprobar comandos básicos
if ! command -v pkg-config >/dev/null 2>&1 && ! command -v pkgconf >/dev/null 2>&1; then
  MISSING_REASONS+=("pkg-config / pkgconf (necesario para detectar librerías de desarrollo)")
fi
if ! command -v clang >/dev/null 2>&1; then
  MISSING_REASONS+=("clang (compilador C/C++)")
fi
if ! command -v ninja >/dev/null 2>&1 && ! command -v samu >/dev/null 2>&1; then
  MISSING_REASONS+=("ninja (sistema de construcción)")
fi
if ! command -v npm >/dev/null 2>&1 && ! command -v corepack >/dev/null 2>&1; then
  MISSING_REASONS+=("nodejs / npm (interfaz web del launcher)")
fi
if ! command -v 7z >/dev/null 2>&1 && ! command -v 7za >/dev/null 2>&1 && ! command -v 7zip >/dev/null 2>&1; then
  MISSING_REASONS+=("7zip / p7zip (para extracción de recursos)")
fi
if ! command -v curl >/dev/null 2>&1; then
  MISSING_REASONS+=("curl")
fi
if ! command -v unzip >/dev/null 2>&1; then
  MISSING_REASONS+=("unzip")
fi

# Comprobar librerías compartidas con pkg-config (universal en cualquier distro)
if command -v pkg-config >/dev/null 2>&1 || command -v pkgconf >/dev/null 2>&1; then
  if ! pkg-config --exists webkit2gtk-4.1 2>/dev/null && ! pkg-config --exists webkit2gtk-4.0 2>/dev/null; then
    MISSING_REASONS+=("webkit2gtk-4.1 / webkit2gtk-4.0 (motor web de la interfaz)")
  fi
  if ! pkg-config --exists gtk+-3.0 2>/dev/null; then
    MISSING_REASONS+=("gtk+-3.0 (ventanas del launcher)")
  fi
  HAS_APPINDICATOR=false
  if pkg-config --exists ayatana-appindicator3-0.1 2>/dev/null || pkg-config --exists appindicator3-0.1 2>/dev/null; then
    HAS_APPINDICATOR=true
  elif ldconfig -p 2>/dev/null | grep -qE "libayatana-appindicator3\.so|libappindicator3\.so"; then
    HAS_APPINDICATOR=true
  elif ls /usr/lib64/libayatana-appindicator3.so* /usr/lib/libayatana-appindicator3.so* /usr/lib/*-linux-gnu/libayatana-appindicator3.so* /usr/lib64/libappindicator3.so* /usr/lib/libappindicator3.so* 2>/dev/null | grep -q .; then
    HAS_APPINDICATOR=true
  fi
  if [ "${HAS_APPINDICATOR}" = "false" ]; then
    MISSING_REASONS+=("libayatana-appindicator / libappindicator (icono en la bandeja del sistema)")
  fi
  if ! pkg-config --exists librsvg-2.0 2>/dev/null && ! ls /usr/lib64/librsvg-2.so* /usr/lib/librsvg-2.so* /usr/lib/*-linux-gnu/librsvg-2.so* 2>/dev/null | grep -q .; then
    MISSING_REASONS+=("librsvg-2.0 (renderizado de iconos vectoriales)")
  fi
  if ! pkg-config --exists openssl 2>/dev/null; then
    MISSING_REASONS+=("openssl (librería criptográfica)")
  fi
  if ! pkg-config --exists dbus-1 2>/dev/null; then
    MISSING_REASONS+=("dbus-1 (comunicación entre procesos)")
  fi
fi

if [ ${#MISSING_REASONS[@]} -gt 0 ]; then
  echo ""
  echo "⚠️  Se detectaron dependencias de desarrollo pendientes en tu sistema:"
  for r in "${MISSING_REASONS[@]}"; do
    echo "    - ${r}"
  done
  echo ""

  if [ -n "${PKG_MANAGER}" ] && [ -n "${INSTALL_CMD}" ]; then
    echo "Gestor detectado: ${PKG_MANAGER}"
    echo "Comando sugerido: ${INSTALL_CMD} ${DISTRO_PKGS[*]}"
    echo ""
    read -r -p "¿Deseas instalar estas dependencias automáticamente con sudo? (s/N): " respuesta
    case "${respuesta}" in
      [sS]|[yY])
        echo "==> Instalando dependencias del sistema con ${PKG_MANAGER}..."
        eval "${INSTALL_CMD} ${DISTRO_PKGS[*]}"
        ;;
      *)
        echo "⚠️  Continuando sin instalar. Si falla la compilación del launcher, ejecuta el comando anterior manualmente."
        ;;
    esac
  else
    echo "ℹ️  Instala las librerías equivalentes con el gestor de paquetes de tu distribución."
  fi
else
  echo "✅ Dependencias del sistema: Todas instaladas y listas."
fi
echo ""

# 4. Comprobar dependencias de Node.js en launcher/
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

# 5. Comprobar ReXGlue SDK (necesario para recompilar el juego y el runtime)
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

# 6. Comprobar y compilar el Runtime automáticamente si default.xex está listo
TARGET_AZURA="${HOME}/.local/share/com.gears3azura.launcher/runtime/azura"
if [ ! -f "${TARGET_AZURA}" ]; then
  if [ ! -f "${ROOT}/runtime/game/default.xex" ]; then
    SEARCH_DIRS=()
    if command -v xdg-user-dir >/dev/null 2>&1; then
      DESK_PATH="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
      DOWN_PATH="$(xdg-user-dir DOWNLOAD 2>/dev/null || true)"
      [ -n "${DESK_PATH}" ] && [ -d "${DESK_PATH}" ] && SEARCH_DIRS+=("${DESK_PATH}")
      [ -n "${DOWN_PATH}" ] && [ -d "${DOWN_PATH}" ] && SEARCH_DIRS+=("${DOWN_PATH}")
    fi
    for d in "${HOME}/Desktop" "${HOME}/Downloads" "${HOME}/Escritorio" "${HOME}/Descargas" "/media" "/run/media"; do
      [ -d "${d}" ] && SEARCH_DIRS+=("${d}")
    done

    FOUND_ISO="$(find "${SEARCH_DIRS[@]}" -maxdepth 3 \( -iname "*spain*.iso" -o -iname "*gears*.iso" \) 2>/dev/null | head -n 1 || true)"
    if [ -z "${FOUND_ISO}" ]; then
      FOUND_ISO="$(find "${SEARCH_DIRS[@]}" -maxdepth 3 -iname "*.iso" 2>/dev/null | head -n 1 || true)"
    fi
    if [ -n "${FOUND_ISO}" ]; then
      echo "==> Detectada imagen ISO en: ${FOUND_ISO}"
      echo "==> Extrayendo default.xex automáticamente..."
      mkdir -p "${ROOT}/runtime/game"
      if [ -x "${ROOT}/tools/extract-xiso" ]; then
        (cd "${ROOT}/runtime/game" && "${ROOT}/tools/extract-xiso" -x -q "${FOUND_ISO}" 2>/dev/null) || true
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
        echo "✅ default.xex extraído correctamente."
      fi
    fi
  fi

  if [ -f "${ROOT}/runtime/game/default.xex" ] && command -v clang >/dev/null 2>&1 && (command -v ninja >/dev/null 2>&1 || command -v samu >/dev/null 2>&1); then
    echo "=========================================================="
    echo "   Compilando Runtime azura para Linux...                "
    echo "=========================================================="
    "${ROOT}/scripts/build.sh" || echo "⚠️  No se pudo completar la compilación del runtime automáticamente."
    echo ""
  fi
fi

# 7. Crear/actualizar acceso directo de escritorio en Linux
cat <<EOF > "${ROOT}/Gears3-Azura.desktop"
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
chmod +x "${ROOT}/Gears3-Azura.desktop" 2>/dev/null || true

DESKTOP_DIR="${HOME}/.local/share/applications"
if [ -d "${DESKTOP_DIR}" ]; then
  cp -f "${ROOT}/Gears3-Azura.desktop" "${DESKTOP_DIR}/azura-launcher.desktop" 2>/dev/null || true
  chmod +x "${DESKTOP_DIR}/azura-launcher.desktop" 2>/dev/null || true
  echo "✅ Acceso directo actualizado en el menú de aplicaciones del sistema."
  echo ""
fi

# 8. Corregir automáticamente valores incompatibles de present_effect ('cas'/'fsr') y GPU
for cfg in "${HOME}/.local/share/com.gears3azura.launcher/runtime/azura.toml" "${ROOT}/runtime/azura.toml"; do
  if [ -f "${cfg}" ]; then
    sed -i -E 's/present_effect[[:space:]]*=[[:space:]]*"(cas|fsr|fsr2|fsr3)"/present_effect = "bilinear"/g' "${cfg}" 2>/dev/null || true
    sed -i '/video_driver/d' "${cfg}" 2>/dev/null || true
    sed -i '/anisotropic_override/d' "${cfg}" 2>/dev/null || true
    sed -i '/texture_cache_memory_limit/d' "${cfg}" 2>/dev/null || true
    if [ ! -e "/dev/nvidiactl" ]; then
      sed -i -E 's/vulkan_device[[:space:]]*=[[:space:]]*[0-9]+/vulkan_device = -1/g' "${cfg}" 2>/dev/null || true
    fi
  fi
done

# 9. Iniciar el launcher
echo "=========================================================="
echo "   Iniciando Gears3-Azura Launcher...                    "
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
