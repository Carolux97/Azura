# Contributing to Gears3-Azura

Gears3-Azura runs on macOS and Linux from one codebase. Every change goes through a Trello card, a
short-lived branch and a pull request to `main`, reviewed and tested on both platforms before it is
merged. Everything written into the repository (code, comments, commit messages, docs) and on the
Trello board is in English.

## Workflow

1. **Card.** Every piece of work starts as a card on the Trello board, in the format below.
2. **Branch.** Create a branch from an up-to-date `main`, named after the kind of change:
   `feat/…`, `fix/…`, `perf/…`, `chore/…` or `docs/…` (for example `fix/wasd-glyph`). One card, one
   branch.
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

## Commits

- Messages in English, starting with the area or areas they touch, then a summary in the
  imperative: `runtime: order the render thread's reads of the command queue`,
  `runtime, launcher: let the launcher restart the game`.
- The body explains what changed and why.
- No AI assistant as co-author and no "generated with" lines, in commits or pull requests.
- No card or ticket references in code or comments; they go in commit messages and pull requests.
- Comments describe what the code does now and why, not the history of a fix.

## Before opening a pull request

- Launcher tests pass: `cargo test` in `launcher/src-tauri` and `npm test` in `launcher`.
- The runtime builds with `scripts/build.sh`.
- The diff has no game data, no generated code (`runtime/generated/`) and no signing keys.
