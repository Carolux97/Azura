# Contributing to Gears3-Azura

Gears3-Azura runs on macOS, Linux and Windows from one codebase. Every change goes through a Trello card, a
short-lived branch and a pull request to `main`, reviewed and tested on both platforms before it is
merged. Everything written into the repository (code, comments, commit messages, docs) and on the
Trello board is in English.

## Contributions from outside the team

The Trello board where the team tracks its work is not public, so the workflow below is for the
team. If you would like to contribute:

1. **Open an issue first**, describing the bug (with the launcher's log, your system and your game
   edition) or the change you have in mind, and wait for an answer before writing code: someone may
   already be on it, or it may not fit the project.
2. **Then a pull request**, from a branch of your fork, that names the issue in its description
   instead of a card. Its checks run once a maintainer approves them.

Contributions are accepted under the terms in [License of contributions](#license-of-contributions).

## Workflow

1. **Card.** Every piece of work starts as a card on the Trello board, in the format below.
2. **Branch.** Create a branch from an up-to-date `main`, named as described in
   [Branches](#branches). One card, one branch.
3. **Pull request.** Open a pull request to `main`. GitHub fills its description from
   [.github/pull_request_template.md](.github/pull_request_template.md); link the card there and the
   pull request in the card's Notes.
4. **Review and test.** The other developer reviews it and tests it on their platform: a change made
   on macOS is tested on Linux, and the other way round. A change that only affects one platform
   says so in the pull request.
5. **Merge.** Once approved and tested, the pull request is merged into `main` and its card moves to
   Done.

The board's lists follow that flow:

- **Backlog**: everything noted down, even if it is still half thought through.
- **Ready**: cards that can be started without asking anything: the description follows the format
  below, the acceptance criteria are clear and no decision is pending. Moving a card from Backlog to
  Ready means refining it: completing the format and settling its open questions.
- **In progress**: someone is working on it.
- **In review**: its pull request is open.
- **Done**: merged into `main`.

A card that cannot move forward keeps its list and gets the **Blocked** label, together with a
comment saying what blocks it and who can unblock it. The label comes off once it is unblocked.

## Branches

`type/card-number-short-description`, for example `fix/6-wasd-glyph`, `perf/1-guest-fences` or
`chore/11-launcher-ci`.

- **Type**, from the card's type label: Feature → `feat/`, Bug → `fix/`, Performance → `perf/`,
  Tooling → `chore/`, Research → `research/`; documentation-only changes use `docs/`. When a card
  has several type labels, use the main one.
- **Card number**: the number the Card Numbers Power-Up shows on the card. It does not change when
  the card is renamed or moved, so every branch leads back to its card.
- **Short description**: English, lowercase, words separated by hyphens, two to four words; no
  spaces, accents or capitals.
- A small change without a card (a typo in the docs, for example) drops the number:
  `docs/fix-readme-typo`. This is the exception.
- Branches are short-lived: delete them once their pull request is merged.

## Trello cards

### Title

`Area: outcome in a few words`, where the area is `Runtime`, `Launcher`, `Build` or `Tooling`. For
example: `Runtime: WASD glyph too small in HUD prompts`.

### Description

```markdown
## Context
What happens today and what we already know.

## Goal
What we want to achieve.

## Acceptance criteria
- When the card can be closed, measurable when possible.

## Notes
Hints, links, what was already tried, related pull requests.
```

A **Research** card ends with a written proposal (options, risks, recommendation) instead of code;
implementing it gets its own card.

### Labels

Every card has at least one label of each of the type, platform and area groups. A change that affects both platforms has both
platform labels.

| Group    | Label       | Colour | Use                                                   |
|----------|-------------|--------|-------------------------------------------------------|
| Type     | Feature     | green  | New behaviour                                         |
|          | Bug         | red    | Something that works wrong                            |
|          | Performance | orange | Speed, frame rate, memory                             |
|          | Tooling     | purple | Build, CI, workflow                                   |
|          | Research    | sky    | Investigate or design before implementing             |
| Platform | macOS       | black  | Affects macOS                                         |
|          | Linux       | yellow | Affects Linux                                         |
| Area     | Runtime     | blue   | The recompiled game and our hooks (`runtime/`)        |
|          | Launcher    | pink   | The Tauri launcher (`launcher/`)                      |
|          | Build       | lime   | Build scripts, CMake, SDK patches (`scripts/`, `patches/`) |
| Status   | Blocked     | dark red | Cannot move forward; the card's comments say why    |

### Priority, effort and members

- **Priority**: Highest (blocks other work or breaks `main`), High (next to do), Medium (important
  but can wait), Low (when there is time), Lowest (only if time is left over), Not sure (not
  enough information to decide).
- **Effort**, in Fibonacci points that measure complexity and uncertainty, not hours: 1 a small,
  clear change; 2-3 a contained change in a known place; 5 several pieces or some research; 8 large
  or uncertain; 13 too large, split the card.
- Every card in Ready has a priority and an effort.
- **Member**: one per card, the person who will do it. A card whose owner is not decided has no
  member.

## Commits

- Messages in English, starting with the area or areas they touch, then a summary in the
  imperative: `runtime: order the render thread's reads of the command queue`,
  `runtime, launcher: let the launcher restart the game`.
- The body explains what changed and why.
- No AI assistant as co-author and no "generated with" lines, in commits or pull requests.
- No card or ticket references in code or comments; they go in commit messages and pull requests.
- Comments describe what the code does now and why, not the history of a fix.

## License of contributions

The project's own code is under the PolyForm Strict License 1.0.0 (see [LICENSE](LICENSE)), which
does not let others change or share it. By sending a contribution you agree that it is licensed
under the same terms, and that Carolina Molina Elvira and Juan Llorca Martínez, who hold the
project's copyright, may use, change and share it as part of the project under any license.
Changes to the ReXGlue SDK and MoltenVK (`patches/`) stay under those projects' licenses.

## Before opening a pull request

- Launcher tests pass: `cargo test` in `launcher/src-tauri` and `npm test` in `launcher`.
- The runtime builds with `scripts/build.sh`.
- The diff has no game data, no generated code (`runtime/generated/`) and no signing keys.

## Continuous integration

The workflows run on GitHub's runners (`macos-latest` and `ubuntu-latest`; Ubuntu 22.04 for the
Linux kit, in a container, and the AppImage, so they need no newer glibc than 2.35): Launcher
tests on pull requests that change the launcher, Runtime kit on pull requests that change what
defines the kit, and the Release workflow on main when the launcher's version changes, or by hand.
A release whose Linux or macOS build failed publishes the other platform; run the Release
workflow by hand with "Publish" on to add the missing one to that version's release.
