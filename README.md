# Gears3-Azura

Unofficial native port of Gears of War 3 (Xbox 360) to macOS on Apple Silicon and to Linux
(x86-64).

It does not emulate the console at run time. The game's PowerPC code is translated ahead of time
into C++ by the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled into a normal
native executable. ReXGlue also provides the runtime that stands in for the console's operating
system and GPU; on macOS the GPU work goes through our own Metal backend, on Linux through
Vulkan.

**This repository contains no game data and no game code.** You need your own copy of the game:
the recompiled code is generated on your machine from your own `default.xex` and is never committed.

## Download

The launcher for macOS (Apple Silicon) and Linux (x86-64) is published in
[gears3-azura-releases](https://github.com/Carolux97/gears3-azura-releases/releases/latest).
Point it at your own copy of the game (a folder or a disc image): it builds the game for your
machine and keeps itself and the game up to date. The macOS launcher is not signed with an Apple
Developer ID, so the first time it opens from System Settings, Privacy & Security.

## Layout

| Path | What it holds |
| --- | --- |
| `versions.toml` | The game builds the project supports, identified by the SHA-256 of their `default.xex`. Shared by the launcher and the build script. |
| `runtime/` | The game runtime: one ReXGlue manifest per supported build (`azura_<media ID>.toml`), our C++ hooks (`src/`), default settings and controller mappings (`config/`), tests (`tests/`). |
| `runtime/game/` | Your own game files (`default.xex` and the data folders), for building from source. Ignored by git. |
| `runtime/generated/` | C++ produced by `rexglue codegen` from your `default.xex`. Ignored by git. |
| `launcher/` | The desktop launcher (Tauri): interface in `launcher/src/`, native side in `launcher/src-tauri/`. See its README. |
| `patches/` | Our changes to the ReXGlue SDK (the Metal backend, the Xenos shader compiler, the PowerPC interpreter and fixes) and to MoltenVK, applied by the build scripts. |
| `scripts/` | Build, player kit and release scripts. |
| `tools/` | [extract-xiso](https://github.com/XboxDev/extract-xiso), to extract disc images. |
| `docs/` | Design notes: the [Metal backend](docs/metal-backend-design.md), the [Xenos shader compiler](docs/shader-compiler-design.md), the [PowerPC interpreter](docs/ppc-interpreter-design.md), [building the runtime on the player's machine](docs/player-runtime-build.md), [Linux GPU measurements](docs/linux-gpu-measurement.md) and the game's internals for modding menus and settings ([reverse-engineering.md](docs/reverse-engineering.md)). |
| `sdk/` | The ReXGlue SDK: its prebuilt release for your platform and its source tree, which the build patches. Ignored by git. |
| `iniciar-linux.sh` | Sets up and starts the launcher from a checkout on Linux. |

## Requirements

To play, you only need the launcher (see Download), on macOS 13.3 or later on Apple Silicon, or on
Linux x86-64 with glibc 2.35 or newer (as Ubuntu 22.04), and your own copy of the game.

To build from source:

- On macOS: Apple Silicon, Xcode Command Line Tools (Apple clang 18 or newer), CMake 3.25+ and Ninja
  (`brew install cmake ninja`).
- On Linux: clang, CMake 3.25+, Ninja, the Vulkan SDK and the development libraries the SDK builds
  against (the "Install the toolchain (Linux)" step of `.github/workflows/runtime-kit.yml` lists
  them for Ubuntu 22.04); `iniciar-linux.sh` sets up the launcher.
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

## Status

The single-player campaign runs on macOS (Metal) and Linux (Vulkan), and we keep playing it through
to fix what turns up. Working today: controllers (Xbox, PlayStation and others, with their button
prompts in the game), keyboard and mouse, achievements with their artwork, a save manager with
backups, and video options (render resolution, upscaling, antialiasing, frame rate limit). The
launcher builds the game from your own copy and keeps itself and the game up to date. Not yet:
co-op and multiplayer.

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
