#!/usr/bin/env bash
# Builds a signed release of the Azura launcher for macOS (Apple Silicon) and, with --publish,
# publishes it on GitHub so installed launchers update themselves.
#
# Steps:
#   1. read the version from tauri.conf.json and check package.json and Cargo.toml agree; the
#      script never changes the version, bump it in those three files first;
#   2. build the app and the .dmg, plus the signed update archive (.app.tar.gz and its .sig);
#   3. copy them to launcher/dist/<version>/ with space-free names and write latest.json, the
#      file the launcher's updater reads (version, notes, date, archive URL and signature);
#   4. with --publish: create the GitHub release "launcher-v<version>" with the archive and the
#      .dmg, and upload latest.json to the fixed release "launcher-updates", which is where the
#      updater endpoint in tauri.conf.json points.
#
# The update signing key never lives in the repository. Pass it through the environment:
#   TAURI_SIGNING_PRIVATE_KEY           path to the private key (or its contents)
#   TAURI_SIGNING_PRIVATE_KEY_PASSWORD  its password
# Optional:
#   LAUNCHER_RELEASE_NOTES  text shown with the update (default: none)
#   AZURA_UPDATE_BASE_URL   base URL of the archive in latest.json, for testing with a local
#                           server (default: the GitHub release download URL)
#
# Usage: scripts/release-launcher.sh [--publish]

set -euo pipefail

# Ensure cargo and rustup toolchains are in PATH
if ! command -v cargo &>/dev/null; then
  if [[ -d "${HOME}/.rustup/toolchains/stable-aarch64-apple-darwin/bin" ]]; then
    export PATH="${HOME}/.rustup/toolchains/stable-aarch64-apple-darwin/bin:${PATH}"
  elif [[ -d "${HOME}/.cargo/bin" ]]; then
    export PATH="${HOME}/.cargo/bin:${PATH}"
  fi
fi

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly LAUNCHER="${ROOT}/launcher"
readonly REPO="Carolux97/Gears3-Azura"
readonly UPDATES_TAG="launcher-updates"
readonly PLATFORM="darwin-aarch64"

PUBLISH=false
case "${1:-}" in
  "") ;;
  --publish) PUBLISH=true ;;
  *)
    echo "Unknown option '$1'. Usage: scripts/release-launcher.sh [--publish]" >&2
    exit 1
    ;;
esac

# Prints a JSON field of a file with Node, which the launcher already requires.
json_field() {
  node -e 'process.stdout.write(String(require(process.argv[1])[process.argv[2]]))' "$1" "$2"
}

readonly VERSION="$(json_field "${LAUNCHER}/src-tauri/tauri.conf.json" version)"
readonly PACKAGE_VERSION="$(json_field "${LAUNCHER}/package.json" version)"
readonly CARGO_VERSION="$(sed -n 's/^version = "\(.*\)"$/\1/p' "${LAUNCHER}/src-tauri/Cargo.toml" | head -n 1)"
if [[ "${VERSION}" != "${PACKAGE_VERSION}" || "${VERSION}" != "${CARGO_VERSION}" ]]; then
  echo "Versions differ: tauri.conf.json ${VERSION}, package.json ${PACKAGE_VERSION}, Cargo.toml ${CARGO_VERSION}." >&2
  exit 1
fi

if [[ -z "${TAURI_SIGNING_PRIVATE_KEY:-}" ]]; then
  echo "TAURI_SIGNING_PRIVATE_KEY is not set. See the comment at the top of this script." >&2
  exit 1
fi

readonly TAG="launcher-v${VERSION}"
if ${PUBLISH} && gh release view "${TAG}" --repo "${REPO}" >/dev/null 2>&1; then
  echo "Release ${TAG} already exists. Bump the version first." >&2
  exit 1
fi

echo "==> Cleaning old bundles in target"
rm -rf "${LAUNCHER}/src-tauri/target/debug/bundle"
rm -rf "${LAUNCHER}/src-tauri/target/release/bundle/dmg"

echo "==> Building Azura Launcher ${VERSION}"
(cd "${LAUNCHER}" && npx tauri build --bundles app,dmg --config '{"bundle":{"createUpdaterArtifacts":true}}')

readonly BUNDLE="${LAUNCHER}/src-tauri/target/release/bundle"
readonly DIST="${LAUNCHER}/dist/${VERSION}"
readonly ARCHIVE="Azura-Launcher_${VERSION}_aarch64.app.tar.gz"
readonly DMG="Azura-Launcher_${VERSION}_aarch64.dmg"
rm -rf "${DIST}"
mkdir -p "${DIST}"
cp "${BUNDLE}/macos/Azura Launcher.app.tar.gz" "${DIST}/${ARCHIVE}"
cp "${BUNDLE}/macos/Azura Launcher.app.tar.gz.sig" "${DIST}/${ARCHIVE}.sig"

readonly GENERATED_DMG="$(find "${BUNDLE}/dmg" -name "*.dmg" -print -quit)"
cp "${GENERATED_DMG}" "${DIST}/${DMG}"
# Keep target clean: only one canonical Azura Launcher.dmg in target/release/bundle/dmg/
mv -f "${GENERATED_DMG}" "${BUNDLE}/dmg/Azura Launcher.dmg"

# Clean ghost registrations in macOS LaunchServices caused by temporary DMG mount volumes
if [[ -x "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister" ]]; then
  readonly LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
  "${LSREGISTER}" -dump 2>/dev/null \
    | awk '/path:[[:space:]]*\/Volumes\/.*Azura Launcher\.app/ {sub(/^[[:space:]]*path:[[:space:]]*/, ""); sub(/[[:space:]]*\(0x[0-9a-f]+\)$/, ""); print}' \
    | while IFS= read -r p; do
        "${LSREGISTER}" -u "${p}" 2>/dev/null || true
      done
  "${LSREGISTER}" -gc 2>/dev/null || true
fi

readonly BASE_URL="${AZURA_UPDATE_BASE_URL:-https://github.com/${REPO}/releases/download/${TAG}}"
echo "==> Writing latest.json"
node -e '
  const fs = require("fs");
  const [file, version, notes, platform, url, sigFile] = process.argv.slice(1);
  const manifest = {
    version,
    notes,
    pub_date: new Date().toISOString(),
    platforms: { [platform]: { signature: fs.readFileSync(sigFile, "utf8").trim(), url } },
  };
  fs.writeFileSync(file, JSON.stringify(manifest, null, 2) + "\n");
' "${DIST}/latest.json" "${VERSION}" "${LAUNCHER_RELEASE_NOTES:-}" "${PLATFORM}" \
  "${BASE_URL}/${ARCHIVE}" "${DIST}/${ARCHIVE}.sig"

if ! ${PUBLISH}; then
  echo "==> Done (not published). Files in ${DIST}/"
  exit 0
fi

echo "==> Publishing ${TAG}"
gh release create "${TAG}" --repo "${REPO}" --title "Azura Launcher ${VERSION}" \
  --notes "${LAUNCHER_RELEASE_NOTES:-Azura Launcher ${VERSION}}" \
  "${DIST}/${ARCHIVE}" "${DIST}/${DMG}"

if ! gh release view "${UPDATES_TAG}" --repo "${REPO}" >/dev/null 2>&1; then
  gh release create "${UPDATES_TAG}" --repo "${REPO}" --title "Launcher update feed" --latest=false \
    --notes "Holds latest.json, which installed launchers read to update themselves."
fi
gh release upload "${UPDATES_TAG}" --repo "${REPO}" --clobber "${DIST}/latest.json"

echo "==> Published ${TAG}"
