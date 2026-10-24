#!/usr/bin/env bash
# Publishes a launcher release that scripts/release-launcher.sh built, so installed launchers
# update themselves:
#   1. merge the platforms' entries (latest-<platform>.json) into latest.json, the file the
#      launcher's updater reads (version, notes, date, and per platform the archive's URL and
#      signature);
#   2. create the release "launcher-v<version>" in the releases repository (release-common.sh)
#      with the downloads and the update archives;
#   3. upload latest.json to the fixed release "launcher-updates", where the updater endpoint in
#      tauri.conf.json points.
#
# A release may hold only the platforms that built. Publishing the same version again adds the
# platforms in <dist folder> to its release (replacing their files) and merges their entries into
# the published latest.json, keeping the platforms already there.
#
# Releases are titled with the version alone and carry no description of the project: the
# releases repository is public, and is kept as hard to come across as it can be.
#
# gh needs write access to the releases repository: a local `gh auth login`, or GH_TOKEN (the
# Release workflow passes a token that can only write to that repository).
# The notes shown with the update and on the release are the version's section of
# launcher/CHANGELOG.md.
#
# Usage:
#   scripts/publish-launcher.sh <dist folder>   publish the release built into that folder
#   scripts/publish-launcher.sh --status        print "released" or "new" for the current version
#                                               (failing for a new one without its notes)

set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/scripts/release-common.sh"

# Assigned before readonly so a failed check stops the script (set -e).
VERSION="$(launcher_version)"
readonly VERSION
readonly TAG="$(release_tag "${VERSION}")"

if [[ "${1:-}" == "--status" ]]; then
  if release_exists "${TAG}"; then
    echo released
  else
    # A new version needs its notes before anything is built for it.
    release_notes "${VERSION}" >/dev/null
    echo new
  fi
  exit 0
fi

readonly DIST="${1:?Usage: scripts/publish-launcher.sh <dist folder> | --status}"
shopt -s nullglob
readonly ENTRIES=("${DIST}"/latest-*.json)
NOTES="$(release_notes "${VERSION}")"
readonly NOTES
if (( ${#ENTRIES[@]} == 0 )); then
  echo "No latest-<platform>.json in ${DIST}: build the release first (scripts/release-launcher.sh)." >&2
  exit 1
fi
RELEASED=false
release_exists "${TAG}" && RELEASED=true
readonly RELEASED

# The published latest.json of this version, whose platforms are kept when adding others; an
# empty file for a new version (or a published one of an older version, which is replaced).
PUBLISHED="$(mktemp)"
readonly PUBLISHED
trap 'rm -f "${PUBLISHED}"' EXIT
if [[ "${RELEASED}" == true ]]; then
  gh release download "${UPDATES_TAG}" --repo "${RELEASES_REPO}" --pattern latest.json \
    --output "${PUBLISHED}" --clobber 2>/dev/null || : >"${PUBLISHED}"
fi

echo "==> Writing latest.json"
node -e '
  const fs = require("fs");
  const [file, published, version, notes, ...entries] = process.argv.slice(1);
  const text = fs.readFileSync(published, "utf8");
  const previous = text ? JSON.parse(text) : null;
  const kept = previous?.version === version ? previous.platforms : {};
  const platforms = Object.assign({}, kept, ...entries.map((entry) => JSON.parse(fs.readFileSync(entry, "utf8"))));
  const manifest = { version, notes, pub_date: previous?.version === version ? previous.pub_date : new Date().toISOString(), platforms };
  fs.writeFileSync(file, JSON.stringify(manifest, null, 2) + "\n");
' "${DIST}/latest.json" "${PUBLISHED}" "${VERSION}" "${NOTES}" "${ENTRIES[@]}"

# Every file of the release but the updater's own: the signatures are inside latest.json.
FILES=()
for file in "${DIST}"/*; do
  case "${file}" in
    *.json | *.sig) ;;
    *) FILES+=("${file}") ;;
  esac
done

if [[ "${RELEASED}" == true ]]; then
  echo "==> Adding to ${TAG} in ${RELEASES_REPO}"
  gh release upload "${TAG}" --repo "${RELEASES_REPO}" --clobber "${FILES[@]}"
else
  echo "==> Publishing ${TAG} to ${RELEASES_REPO}"
  gh release create "${TAG}" --repo "${RELEASES_REPO}" --title "${VERSION}" \
    --notes "${NOTES}" "${FILES[@]}"
fi

if ! release_exists "${UPDATES_TAG}"; then
  gh release create "${UPDATES_TAG}" --repo "${RELEASES_REPO}" --title "updates" --latest=false --notes "updates"
fi
gh release upload "${UPDATES_TAG}" --repo "${RELEASES_REPO}" --clobber "${DIST}/latest.json"

echo "==> Published ${TAG}"
