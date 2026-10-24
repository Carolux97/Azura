# Shared by release-launcher.sh and publish-launcher.sh (sourced, not run): where releases are
# published and the launcher's version.
#
# Releases go to a public repository that only holds releases, so installed launchers can download
# them while this repository stays private. Its name is also in the updater endpoint of
# launcher/src-tauri/tauri.conf.json; change both together.

readonly RELEASES_REPO="Carolux97/files"

# The fixed release whose latest.json installed launchers read (the updater endpoint).
readonly UPDATES_TAG="launcher-updates"

# Prints a JSON field of a file with Node, which the launcher already requires.
json_field() {
  node -e 'process.stdout.write(String(require(process.argv[1])[process.argv[2]]))' "$1" "$2"
}

# Prints the launcher's version after checking tauri.conf.json, package.json and Cargo.toml agree.
launcher_version() {
  local launcher="${ROOT}/launcher"
  local version package_version cargo_version
  version="$(json_field "${launcher}/src-tauri/tauri.conf.json" version)"
  package_version="$(json_field "${launcher}/package.json" version)"
  cargo_version="$(sed -n 's/^version = "\(.*\)"$/\1/p' "${launcher}/src-tauri/Cargo.toml" | head -n 1)"
  if [[ "${version}" != "${package_version}" || "${version}" != "${cargo_version}" ]]; then
    echo "Versions differ: tauri.conf.json ${version}, package.json ${package_version}, Cargo.toml ${cargo_version}. Bump all three to the same version." >&2
    return 1
  fi
  printf '%s' "${version}"
}

# Prints the list items of a version's section of launcher/CHANGELOG.md, as the launcher shows them
# (src-tauri/src/release_notes.rs reads the same file); fails when the section is missing or empty.
release_notes() {
  local notes
  notes="$(awk -v heading="## $1" '
    $0 == heading { inside = 1; next }
    inside && /^## / { exit }
    inside && /^- / { print; next }
    inside && /^  [^ ]/ { sub(/^ +/, "  "); print }
  ' "${ROOT}/launcher/CHANGELOG.md")"
  if [[ -z "${notes}" ]]; then
    echo "launcher/CHANGELOG.md has no notes for version $1: add its section first." >&2
    return 1
  fi
  printf '%s\n' "${notes}"
}

# The release tag of a version.
release_tag() {
  printf 'launcher-v%s' "$1"
}

# Where a release's files are downloaded from.
release_download_url() {
  printf 'https://github.com/%s/releases/download/%s' "${RELEASES_REPO}" "$1"
}

# Succeeds when the release of a tag is already published.
release_exists() {
  gh release view "$1" --repo "${RELEASES_REPO}" >/dev/null 2>&1
}
