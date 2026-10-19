# Gears3-Azura

Unofficial native port of Gears of War 3 (Xbox 360) to macOS on Apple Silicon, with Linux and
Windows as later targets.

It does not emulate the console at run time. The game's PowerPC code is translated ahead of time
into C++ by the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled into a normal
macOS executable. ReXGlue also provides the runtime that stands in for the console's operating
system and GPU.

**This repository contains no game data and no game code.** You need your own copy of the game:
the recompiled code is generated on your machine from your own `default.xex` and is never committed.

## Layout

| Path | What it holds |
| --- | --- |
| `versions.toml` | The game builds the project supports, identified by the SHA-256 of their `default.xex`. Shared by the launcher and the build script. |
| `runtime/` | The game runtime: one ReXGlue manifest per supported build (`azura_<media ID>.toml`), our C++ hooks (`src/`), default settings (`config/`). |
| `runtime/game/` | Your own game files (`default.xex` and the data folders). Ignored by git. |
| `runtime/generated/` | C++ produced by `rexglue codegen` from your `default.xex`. Ignored by git. |
| `launcher/` | The desktop launcher (Tauri): interface in `launcher/src/`, native side in `launcher/src-tauri/`. See its README. |
| `scripts/` | Build scripts. |
| `docs/` | Notes on the game's internals for modding menus and settings: [reverse-engineering.md](docs/reverse-engineering.md). |
| `sdk/` | The ReXGlue SDK for your platform. Ignored by git. |

## Requirements (macOS)

- A Mac with Apple Silicon.
- Xcode Command Line Tools (Apple clang 18 or newer).
- CMake 3.25+ and Ninja: `brew install cmake ninja`.
- ReXGlue SDK **0.10.0**, `mac-arm64` build, unpacked so that
  `sdk/rexglue-0.10.0-mac-arm64/mac-arm64/bin/rexglue` exists. Download it from the
  [v0.10.0 release](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.10.0).
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

Project skeleton. Nothing has been recompiled or run yet.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD 3-Clause), derived from
  [Xenia](https://github.com/xenia-project/xenia).
- [Gears of War: Judgment for PC](https://github.com/OverkillLabs3/gears-judgement-recomp)
  (BSD 3-Clause), used as a reference for the project structure.

Gears of War is a trademark of Microsoft. This project is not affiliated with or endorsed by
Microsoft, Epic Games or The Coalition.

## License

Gears3-Azura's own code is released under the [BSD 3-Clause License](LICENSE). The third-party
code it builds on, and the licenses that apply to it, are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The license covers this project's code only:
Gears of War, its code and its data belong to their owners and are not part of this repository.
