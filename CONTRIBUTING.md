# Contributing to Gears3-Azura

Gears3-Azura runs on macOS and Linux from one codebase. Every change goes through a Trello card, a
short-lived branch and a pull request to `main`, reviewed and tested on both platforms before it is
merged. Everything written into the repository (code, comments, commit messages, docs) and on the
Trello board is in English.

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

## Before opening a pull request

- Launcher tests pass: `cargo test` in `launcher/src-tauri` and `npm test` in `launcher`.
- The runtime builds with `scripts/build.sh`.
- The diff has no game data, no generated code (`runtime/generated/`) and no signing keys.

## Continuous integration

The workflows run on pull requests (Launcher tests when the launcher changes, Runtime kit when
what defines the kit changes) and the Release workflow on main only when the launcher's version
changes, to keep within the GitHub Actions minutes of a private repository, where a macOS minute
counts as ten.

- **macOS jobs, and the small Release jobs** (the version check and the publish), run on the
  project's own Mac: a self-hosted runner with the labels `self-hosted`, `macOS` and `ARM64`, which
  costs no minutes. They wait while that Mac is off or its user is logged out.
- **Linux launcher tests** run on the project's own Linux runner, `casaserver-linux`, with the labels
  `self-hosted`, `Linux` and `X64`: an Ubuntu 22.04 container on Juan Llorca Martínez's TrueNAS
  server (see below). It costs no minutes and waits while that server is off.
- **The other Linux jobs** (the Linux kit and AppImage, which need x86-64 Linux) still run on
  GitHub's Ubuntu runners. While the included minutes are used up they cannot run, and a release
  publishes macOS alone; once Linux can build again, run the Release workflow by hand with
  "Publish" on to add it to that version's release.

To set up a runner (once per machine): in the repository, Settings, Actions, Runners, New
self-hosted runner, pick the machine's system, download the runner it shows into
`~/actions-runner` (outside the repository) and run the `./config.sh` command it gives, which
carries a one-off registration token. Then `./svc.sh install` and `./svc.sh start` make it start
with the session. Jobs run as that user, with its Homebrew, Rust and Node, in
`~/actions-runner/_work`.

The Linux runner lives in `gh-runner`, an unprivileged container (TrueNAS Instances, image Ubuntu
jammy, 3 CPUs, 3 GiB of memory, autostart) that mounts none of the server's datasets, so a job
cannot reach them. To set it up again:

1. In the container, as root: `apt-get install openssh-server sudo curl git` and the libraries the
   Linux launcher job needs (the "Install the Linux libraries Tauri needs" step of
   `launcher-tests.yml`, plus `pkg-config`), and create a user `runner` without sudo. The step
   that installs them is skipped on self-hosted runners, since the runner user cannot run apt.
2. As `runner`, install the runner in `~/actions-runner` as above, with
   `--labels self-hosted,Linux,X64`, and add to its `.env`: `CARGO_HOME` and `RUSTUP_HOME` under
   `~/actions-runner` (so a job's Rust stays apart from anything else), and `CARGO_BUILD_JOBS=2`
   and `CMAKE_BUILD_PARALLEL_LEVEL=2`, so builds leave memory to the server's own apps.
3. As root: `./svc.sh install runner` and `./svc.sh start` in `~runner/actions-runner`.
