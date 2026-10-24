# Launcher

Desktop launcher for Gears3-Azura, built with [Tauri](https://tauri.app): the interface is web code
(HTML, CSS and TypeScript) and a thin Rust layer talks to the operating system.

Planned responsibilities:

- choose where the game files live and check that they are a supported dump;
- install and select translated game files;
- edit the runtime settings (graphics, display, input) that `runtime/` reads from `azura.toml`;
- start the game and show its log if it fails to start;
- check for updates of the launcher and the runtime.

The interface is our own design with a theme inspired by the game. It uses only original artwork
and freely licensed fonts bundled with the app: no logos, fonts or images taken from the game or
from its publishers.

## Languages

The launcher follows the system language when it is one of the languages Gears of War 3 shipped
with (English, Czech, German, Spanish, French, Hungarian, Italian, Japanese, Korean, Polish,
Portuguese, Russian and Traditional Chinese) and falls back to English otherwise. The user can override it from the
language selector. Messages live in `src/locales/<code>.json`; `en.json` is the reference, and
every other file must have the same keys and placeholders:

```bash
npm test
```

## Settings split

Settings live in the app data folder (`~/Library/Application Support/com.gears3azura.launcher/`):
`runtime/azura.toml` holds what the runtime reads, under its own setting names (the game, a folder
or a disc image, is `game_data_root`), and `launcher.toml` what only the launcher uses. The launcher rewrites only its
own keys in `azura.toml`, keeping comments and anything the in-game menu wrote, and reloads the file
when its window regains focus.

The launcher only edits the settings the runtime reads at start-up: the display mode and the
render resolution. The render resolution (`render_resolution`: Automatic, 1280x720, 2560x1440 or
3840x2160) is the same setting as the Render resolution row of the game's Video menu; the runtime
turns it into the draw resolution scale and the texture memory when the game starts, and resolves
Automatic for the machine with the rule in `src/graphics.js` (2560x1440 on Pro, Max and Ultra chips
with at least 16 GB, else 1280x720). Before the first launch after an update, a scale written by an
older launcher (`draw_resolution_scale_x`, with `resolution_auto` in `launcher.toml`) becomes
`render_resolution`. Everything else (frame rate limit, anti-aliasing, motion blur, filtering, image
scaling, mouse sensitivity, bindings) is changed in the game's Options menu.
Both sides read and write the same `azura.toml`.

## Runtime

The runtime is the game recompiled for this computer. Players build it with the launcher from
their own `default.xex` (`src-tauri/src/builder.rs`, `docs/player-runtime-build.md`): the player
kit (`scripts/build-kit.sh`, built in CI) holds everything but the game, and `kit.json` the
commands to build it, which the launcher runs with the machine's compiler. Install in the
Installation view, or Play when nothing is installed, does it: codegen with the kit's code
generator in `build/<game build>/` under the app data folder, the generated files compiled one per
core, the link, and the new runtime folder swapped in by renames, keeping the player's
`azura.toml` and logs. Each object is reused while its key matches (its source, the command, the
compiler and the generated headers the compiler reported it used), so an interrupted build
resumes. Progress arrives as `runtime-build-progress` events.

Updates: `build/<game build>/state.json` records the kit's keys, the game executable's hash, the
compiler and the objects the installed runtime was linked from. When a newer kit only changes what
the link uses (our code, the SDK libraries, the staged files) and the objects are still there, the
runtime is linked again from them in under a second; when the generated code depends on what
changed (the code generator, the manifest, the headers, the commands), or the game or the compiler
changed, it is built again, compiling only the files whose key changed. The Installation view says
which an update takes, and Play installs it before starting the game.

Where the runtime comes from, in order: `AZURA_RUNTIME_BUILD` (a build folder, copied),
`AZURA_KIT` (a kit folder, built from), a project build when the launcher runs from a checkout
(copied, as developers do), then the kit bundled with the launcher or the project's kit build.

`cargo test builds_a_runtime -- --ignored` builds a real runtime with the launcher's code, given
`AZURA_TEST_KIT` (a kit) and `AZURA_TEST_GAME` (a game folder or disc image).

The game is the folder of the player's dump or its disc image (`.iso`), picked with either button
of the Installation view or dropped on it. A disc image is read in place (`src-tauri/src/disc_image.rs`)
and the runtime mounts it itself; see `docs/player-runtime-build.md`, section 5.

## Compiler

The runtime is built on the player's machine from their own `default.xex`
(`docs/player-runtime-build.md`), so the launcher needs a C++ compiler there; the Installation
view shows its state and installs one when it is missing (`src-tauri/src/toolchain.rs`). A
compiler counts only when it compiles and links a small C++23 program. On macOS it is Apple's,
from the Command Line Tools or Xcode; `xcrun` is run only once `xcode-select -p` names a folder,
since on a Mac without them it would open Apple's install dialog by itself. Installing runs
`xcode-select --install`, which opens that dialog for the player, and the launcher checks again
every few seconds until the tools are there or the dialog is closed. On Linux it is the newest
`clang++` (clang 18 or newer) on the PATH; installing runs the distribution's package manager
(apt, dnf, pacman or zypper, found from `/etc/os-release`) through `pkexec`. Image-based systems
(ostree ones such as Bazzite, and SteamOS) get the manual instructions instead. The manual command
is always shown too.

## Achievements

The Achievements section lists the game's achievements with their artwork, the gamerscore earned and
the Embry Star (named after the game's own medal), earned by unlocking all of them. The player picks a
list, a grid of badges or sections by act (`achievementsView` in `launcher.toml`).

Nothing about the achievements ships with the launcher. Their names, descriptions in every language,
gamerscore, flags and artwork are in the XDBF resource of the player's `default.xex`, which is
encrypted, so the launcher asks the runtime: `azura --game_data_root=<game> --export_achievements=<folder>`
loads the executable without a window or a GPU, writes `achievements.json` and `icons/<imageId>.png`,
and exits (`runtime/src/achievement_export.h`). The launcher runs it once per game build and keeps the
result in `achievements/<build>/` under its app data folder. The unlocks and their dates come from the
file the runtime writes in the saves folder, `achievements/<title ID>.toml`. An achievement the
original game hid (its flags lack 0x8) is sent to the interface without its name, description,
gamerscore or artwork until it is unlocked. The section stays locked, with a button to the
Installation view, until there is a valid game folder and an installed runtime to read it.

## Layout

| Path | What it holds |
| --- | --- |
| `src/` | The interface: plain HTML, CSS and JavaScript, no build step. Tauri serves it as is. |
| `src/fonts/` | Bundled OFL fonts and their licenses, generated by `scripts/fetch-fonts.mjs`. |
| `src-tauri/` | The native side (Rust): window, security policy and, later, the commands the interface calls. |
| `test/` | Node tests for the locale, graphics and achievements logic. |

## Requirements

- Node.js 20+ and npm.
- Rust (stable), for example `brew install rustup` and then `rustup default stable`. Homebrew does
  not put rustup on the PATH: add `/opt/homebrew/opt/rustup/bin` to it.

## Run and build

From `launcher/`:

```bash
npm install
npx tauri dev
```

`npx tauri build` produces `Azura Launcher.app` and a `.dmg` under `src-tauri/target/release/bundle/`.

The interface also opens in any browser, which is handy for quick design changes:

```bash
python3 -m http.server 5180 --directory src
```

## Updates and releases

Installed launchers update themselves with Tauri's updater. On start-up the launcher reads
`latest.json` from the fixed GitHub release `launcher-updates` of this repository; when it lists a
newer version, the sidebar offers it, and installing downloads the archive, checks its signature
against `plugins.updater.pubkey` in `src-tauri/tauri.conf.json` and restarts into it. While the
repository is private that file cannot be downloaded, so the check fails quietly and nothing is
offered.

Releases are signed with a key that never enters the repository. Create it once (it asks for a
password; keep both the file and the password in your password manager: losing either means
installed launchers can never be updated again):

```bash
npx tauri signer generate -w ~/.tauri/azura-launcher.key
```

and put the contents of `~/.tauri/azura-launcher.key.pub` in `plugins.updater.pubkey`. Until then
the release script fails (the build checks that key); debug builds and the app itself are fine.

To release: bump the version in `src-tauri/tauri.conf.json`, `package.json` and
`src-tauri/Cargo.toml`, then run

```bash
TAURI_SIGNING_PRIVATE_KEY=~/.tauri/azura-launcher.key TAURI_SIGNING_PRIVATE_KEY_PASSWORD=... ../scripts/release-launcher.sh --publish
```

Without `--publish` it only builds into `dist/<version>/`.

## Current state

The folder picker, the game folder check, the settings files, the hardware detection behind the
"Auto" preset and the game log are real. The Play button starts
`~/Library/Application Support/com.gears3azura.launcher/runtime/azura` and shows its output; until
the installation step builds the runtime there, it reports the runtime as not built yet.

The app icon is drawn in `src-tauri/icons/app-icon.svg`; regenerate every size with
`npx tauri icon src-tauri/icons/app-icon.svg -o src-tauri/icons` (then delete the `android` and
`ios` folders it also creates).
