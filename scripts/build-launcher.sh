#!/usr/bin/env bash
# Builds the Azura Launcher (macOS .app and .dmg).
# Ensures that target/ always keeps only ONE .app and ONE fixed .dmg ("Azura Launcher.dmg"),
# cleanly replacing previous builds and removing stale artifacts on every build/update.
#
# Usage: scripts/build-launcher.sh

set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly LAUNCHER="${ROOT}/launcher"
readonly BUNDLE="${LAUNCHER}/src-tauri/target/release/bundle"
readonly DMG_DIR="${BUNDLE}/dmg"

echo "==> Cleaning old launcher bundles in target..."
rm -rf "${LAUNCHER}/src-tauri/target/debug/bundle"
rm -rf "${DMG_DIR}"
rm -f "${BUNDLE}/macos/"*.tar.gz* 2>/dev/null || true

echo "==> Building Azura Launcher..."
(cd "${LAUNCHER}" && npx tauri build --bundles app,dmg)

echo "==> Normalizing DMG in target..."
GENERATED_DMG="$(find "${DMG_DIR}" -name "*.dmg" ! -name "Azura Launcher.dmg" -print -quit 2>/dev/null || true)"
if [[ -n "${GENERATED_DMG}" ]]; then
  mv -f "${GENERATED_DMG}" "${DMG_DIR}/Azura Launcher.dmg"
fi

echo "==> Success! Target contains single app and single DMG:"
echo "    App: ${BUNDLE}/macos/Azura Launcher.app"
echo "    DMG: ${DMG_DIR}/Azura Launcher.dmg"
