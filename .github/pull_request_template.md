## Trello card

<!-- Link to the card this PR closes. One card per PR. -->

## What and why

<!-- What changes and why: the problem, its root cause for a bug, and the approach chosen. -->

## Changes

<!-- The main pieces, file by file or area by area, for the reviewer. -->

## Platforms

- [ ] Tested on macOS
- [ ] Tested on Linux
- [ ] Tested on Windows
- [ ] Only affects one platform (say which and why)

## How to test

<!-- Steps the reviewer can follow, starting from scripts/build.sh or the launcher. -->

## Risks and side effects

<!-- Anything else this can affect: other modules, settings, saves, performance. "None" if none. -->

## Checklist

- [ ] Launcher tests pass (`cargo test` in launcher/src-tauri, `npm test` in launcher)
- [ ] The runtime builds with scripts/build.sh
- [ ] No game data, generated code or signing keys in the diff
- [ ] Comments describe the current behaviour; no ticket references in code
