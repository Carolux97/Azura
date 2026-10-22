# Third-party notices

Gears3-Azura's own code is under the BSD 3-Clause License (see [LICENSE](LICENSE)). It builds on
and includes work by others, listed here with their licenses.

## Included in this repository

| Path | Component | License |
|------|-----------|---------|
| `patches/rexglue-0.10.0*.patch` | Changes to the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), Copyright (c) 2026 Tom Clay, with portions derived from [Xenia](https://xenia.jp), Copyright (c) 2022 Ben Vanik and Xenia project contributors | BSD 3-Clause (the SDK's license) |
| `patches/moltenvk-*.patch` | Changes to [MoltenVK](https://github.com/KhronosGroup/MoltenVK), Copyright The Brenwill Workshop Ltd. and contributors | Apache License 2.0 |
| `runtime/assets/fonts/Barlow-*.ttf` | [Barlow](https://github.com/jpt/barlow), Copyright 2017 The Barlow Project Authors | SIL Open Font License 1.1, see [runtime/assets/fonts/Barlow-OFL.txt](runtime/assets/fonts/Barlow-OFL.txt) |
| `tools/extract-xiso` | [extract-xiso](https://github.com/XboxDev/extract-xiso), Copyright (c) 2003 in <in@fishtank.com> | BSD-style, see [tools/LICENSE.TXT](tools/LICENSE.TXT) |

## Used by the runtime

The runtime is not distributed in binary form: everyone builds it on their own machine with
`scripts/build.sh`, from their own copy of the game. It links the ReXGlue SDK and the libraries
the SDK bundles, each under its own license, which ships with the SDK's source tree
(`sdk/rexglue-0.10.0-src/thirdparty/`). Among them:

| Component | License |
|-----------|---------|
| ReXGlue SDK (with Xenia-derived code) | BSD 3-Clause |
| MoltenVK, Vulkan Loader, SPIRV-Tools | Apache License 2.0 |
| SDL 3 | zlib |
| FFmpeg (libavutil and libavcodec only, built without GPL parts) | LGPL 2.1 or later |
| Dear ImGui, {fmt}, spdlog, toml++, inja, Vulkan Memory Allocator | MIT |
| glslang, SPIRV-Headers, Vulkan Headers | Khronos and BSD/MIT-style licenses (see each one's license file) |
| utfcpp, Catch2 | Boost Software License 1.0 |
| Tracy | BSD 3-Clause |
| xxHash | BSD 2-Clause |

## Used by the launcher

The launcher is the part of Gears3-Azura distributed in binary form: the macOS `.dmg` and the
Linux packages. It is built with [Tauri](https://tauri.app) and depends on Rust crates and npm
packages listed in `launcher/src-tauri/Cargo.lock` and `launcher/package-lock.json`. The npm
packages are build tools only; the Rust crates compiled into the launcher are almost all under MIT
or Apache License 2.0, plus a few under BSD, Zlib, ISC, Unicode, Unlicense and MPL 2.0. The MPL 2.0
crates (`cssparser`, `cssparser-macros`, `dtoa-short`, `option-ext` and `selectors`) are used
unmodified; their source is available on crates.io.

Every package of the launcher carries these notices in its `licenses` resource folder (on macOS,
`Azura Launcher.app/Contents/Resources/licenses/`):

- `LICENSE` and `THIRD_PARTY_NOTICES.md`, this project's license and this file;
- `rust-crates.txt`, the license texts of every Rust crate compiled into it, generated from
  `Cargo.lock` by `launcher/scripts/generate-licenses.mjs` before each `tauri build`.

The launcher's fonts ship with their SIL Open Font License texts in `launcher/src/fonts/licenses/`.

## Game

Gears of War is a trademark of Microsoft. This project is not affiliated with or endorsed by
Microsoft, Epic Games or The Coalition, and it contains no game data or game code.
