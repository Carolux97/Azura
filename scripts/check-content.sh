#!/usr/bin/env bash
# Fails when the repository tracks anything derived from the game: the game's own files, the C++
# that `rexglue codegen` generates from default.xex, disc images, and GPU captures (which hold the
# game's shaders, textures and draw data). None of it may ever be committed; .gitignore keeps it
# out of a normal `git add`, and this check catches what gets past it (a forced add, a renamed
# folder). Docs may still hold addresses, inferred signatures and our own notes.
#
# Usage: scripts/check-content.sh   (from anywhere inside the repository; CI runs it on every push
# and pull request, .github/workflows/content-check.yml)

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Pathspecs of forbidden content, matched case-insensitively against every tracked file.
forbidden=(
  ':(icase,glob)runtime/generated/**'
  ':(icase,glob)runtime/game/**'
  ':(icase,glob)**/captures/**'
  ':(icase,glob)**/*.xex'
  ':(icase,glob)**/*.iso'
  ':(icase,glob)**/*.gputrace'
  ':(icase,glob)**/*.gputrace/**'
  ':(icase,glob)runtime/src/hle_gfx/*.gen.cpp'
)

matches="$(git ls-files -- "${forbidden[@]}")"

if [[ -n "${matches}" ]]; then
  echo "error: the repository tracks game-derived content, which must never be committed:" >&2
  echo "${matches}" | sed 's/^/  /' >&2
  echo "Remove it from the index with 'git rm --cached <path>' and keep it local." >&2
  exit 1
fi

echo "Content check passed: no game-derived files are tracked."
