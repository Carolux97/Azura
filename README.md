# Gears3-Azura

Unofficial native port of Gears of War 3 (Xbox 360) to macOS on Apple Silicon, Linux (x86-64) and
Windows (x86-64).

It does not emulate the console at run time. The game's PowerPC code is translated ahead of time
into C++ by the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled into a normal
native executable. ReXGlue also provides the runtime that stands in for the console's operating
system and GPU; on macOS the GPU work goes through our own Metal backend, on Linux and Windows
through Vulkan.

**This repository contains no game data and no game code.** You need your own copy of the game:
the recompiled code is generated on your machine from your own `default.xex` and is never committed.

## Download

The launcher for macOS (Apple Silicon), Linux (x86-64) and Windows (x86-64) is published in
[gears3-azura-releases](https://github.com/Carolux97/gears3-azura-releases/releases/latest).
Each release also has `Azura-Launcher_<version>_all-platforms.zip`, with the installers of the three
platforms in one download. Point the launcher at your own copy of the game (a folder or a disc image): it builds the game for your
machine and keeps itself and the game up to date. The macOS launcher is not signed with an Apple
Developer ID, so the first time it opens from System Settings, Privacy & Security.

On Windows the launcher comes as an installer (`Azura-Launcher_<version>_x64-setup.exe`) that
installs it for your user and registers it in Windows' installed apps, where its uninstaller
removes it. It is not signed, so SmartScreen warns the first time: More info, Run anyway. To build
the game it needs LLVM's clang and Visual Studio's Build Tools (the C++ workload), which it
installs with winget when they are missing; the game runs with Vulkan, as on Linux.

## Layout

| Path | What it holds |
| --- | --- |
| `versions.toml` | The game builds the project supports, identified by the SHA-256 of their `default.xex`. Shared by the launcher and the build script. |
| `runtime/` | The game runtime: one ReXGlue manifest per supported build (`azura_<media ID>.toml`), our C++ hooks (`src/`), default settings and controller mappings (`config/`), tests (`tests/`). |
| `runtime/game/` | Your own game files (`default.xex` and the data folders), for building from source. Ignored by git. |
| `runtime/generated/` | C++ produced by `rexglue codegen` from your `default.xex`. Ignored by git. |
| `launcher/` | The desktop launcher (Tauri): interface in `launcher/src/`, native side in `launcher/src-tauri/`. See its README. |
| `patches/` | Our changes to the ReXGlue SDK (the Metal backend, the Xenos shader compiler, the PowerPC interpreter and fixes) and to MoltenVK, applied by the build scripts. |
| `scripts/` | Build, player kit and release scripts; `build-launcher-windows.sh` builds the Windows launcher from Linux. |
| `tools/` | [extract-xiso](https://github.com/XboxDev/extract-xiso), to extract disc images. |
| `docs/` | Design notes: the [Metal backend](docs/metal-backend-design.md), the [Xenos shader compiler](docs/shader-compiler-design.md), the [PowerPC interpreter](docs/ppc-interpreter-design.md), [building the runtime on the player's machine](docs/player-runtime-build.md), [Linux GPU measurements](docs/linux-gpu-measurement.md) and the game's internals for modding menus and settings ([reverse-engineering.md](docs/reverse-engineering.md)). |
| `sdk/` | The ReXGlue SDK: its prebuilt release for your platform and its source tree, which the build patches. Ignored by git. |
| `iniciar-linux.sh` | Sets up and starts the launcher from a checkout on Linux. |

## Requirements

To play, you only need the launcher (see Download), on macOS 13.3 or later on Apple Silicon, on
Linux x86-64 with glibc 2.35 or newer (as Ubuntu 22.04), or on Windows 10 or 11 x86-64 with a
Vulkan graphics driver, and your own copy of the game.

To build from source:

- On macOS: Apple Silicon, Xcode Command Line Tools (Apple clang 18 or newer), CMake 3.25+ and Ninja
  (`brew install cmake ninja`).
- On Linux: clang, CMake 3.25+, Ninja, the Vulkan SDK and the development libraries the SDK builds
  against (the "Install the toolchain (Linux)" step of `.github/workflows/runtime-kit.yml` lists
  them for Ubuntu 22.04); `iniciar-linux.sh` sets up the launcher.
- On Windows: LLVM's clang (`winget install LLVM.LLVM`), Visual Studio 2022 or its Build Tools
  with the C++ workload, CMake 3.25+, Ninja, Python 3 and Git for Windows, whose Git Bash runs the
  scripts (`scripts/build.sh` uses the preset `win-amd64-sdk-source`). Clone this repository and the
  SDK with `git config --global core.autocrlf false` first: the SDK patches apply to files with
  their original line endings.
- To cross-build for Windows on Linux instead: a Linux build first (its `rexglue` runs codegen for the cross build), and
  for the game the CMake preset `win-amd64-cross-sdk-source` with clang, LLVM and
  Microsoft's CRT and SDK as cargo-xwin downloads them (`~/.cache/cargo-xwin/xwin`, or `XWIN_DIR`;
  see `runtime/cmake/toolchain-win-amd64-cross.cmake`); for the launcher Rust, Node.js, LLVM
  (`clang-cl`, `llvm-lib`, `llvm-rc`) and NSIS (`sudo apt install nsis`), with
  `scripts/build-launcher-windows.sh` setting up the rest (see the launcher's README).
- ReXGlue SDK **0.10.0**: its prebuilt release for your platform, unpacked so that
  `sdk/rexglue-0.10.0-<platform>/<platform>/bin/rexglue` exists (from the
  [v0.10.0 release](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0)), and its source
  tree: `git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git sdk/rexglue-0.10.0-src`.
- Your own Gears of War 3 dump, with `default.xex` copied to `runtime/game/default.xex`. It must be
  one of the builds listed in `versions.toml`.

## Supported builds

| Edition | Media ID | Languages |
| --- | --- | --- |
| World | `58122AD6` | English, French, Portuguese (Brazil), Chinese, Korean, Polish, Russian, Czech, Hungarian |
| España | `6EF14DF9` | English, Spanish |

Both editions run the same code (their executables differ only in the signed header), so one
recompilation serves either.

A disc dump is an XGD3 image; [extract-xiso](https://github.com/XboxDev/extract-xiso) extracts it
(`extract-xiso -s -d <destination> <image.iso>`; options go before the image). To add another
edition, see the comment at the end of `versions.toml`.

## Build

```bash
scripts/build.sh
```

On Windows, `scripts/build.sh` runs in Git Bash as elsewhere. The game for Windows can also be
cross-built on Linux after a Linux build, in `runtime/`:

```bash
cmake --preset win-amd64-cross-sdk-source
cmake --build --preset win-amd64-cross-sdk-source-relwithdebinfo
```

Then, on Windows, with the Visual C++ 2015-2022 redistributable installed:
`azura.exe --game_data_root=<game folder or .iso> --user_data_root=<saves> --cache_root=<cache>`.

## Status

The campaign runs on macOS (Metal), Linux and Windows (Vulkan), alone or with two players in split screen
on one computer, and we keep playing it through to fix what turns up. Working today: controllers
(Xbox, PlayStation and others, with their button prompts in the game), keyboard and mouse, local
co-op (Options, Local co-op), achievements with their artwork, a save manager with backups, and
video options (render resolution, upscaling, antialiasing, frame rate limit). The launcher builds
the game from your own copy and keeps itself and the game up to date. Not yet: online co-op and
multiplayer.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD 3-Clause), derived from
  [Xenia](https://github.com/xenia-project/xenia).
- [Gears of War: Judgment for PC](https://github.com/OverkillLabs3/gears-judgement-recomp)
  (BSD 3-Clause), used as a reference for the project structure.

Gears of War is a trademark of Microsoft. This project is not affiliated with or endorsed by
Microsoft, Epic Games or The Coalition.

## License

Gears3-Azura's own code is source-available under the [PolyForm Strict License 1.0.0](LICENSE):
you may read it and use it for noncommercial purposes, but not change it or share it, in source or
built form, without the copyright holders' permission. Copyright 2026 Carolina Molina Elvira and
Juan Llorca Martínez. Our changes to the ReXGlue SDK and MoltenVK (`patches/`) stay under those
projects' own licenses. The third-party code the project builds on, and the licenses that apply to
it, are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The license covers this
project's code only: Gears of War, its code and its data belong to their owners and are not part
of this repository.
